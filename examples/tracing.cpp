#include <memory>
#include <vector>

#include <js/Object.h>
#include <jsapi.h>

#include "boilerplate.h"

// This example illustrates how to safely store GC pointers in the embedding's
// data structures, and vice versa, by implementing appropriate tracing
// mechanisms.
//
// This example covers using strong references where C++ keeps the JS objects
// alive. Weak references use a different implementation strategy that is not
// covered here.

////////////////////////////////////////////////////////////

// An example C++ type that stores an arbitrary JS value.
struct SafeBox {
  // Arbitrary JS value that will need to be traced. The JS::Heap type has a
  // constructor, destructor and write barriers to properly register the
  // pointer with the GC as needed.
  JS::Heap<JS::Value> stashed;

  // The JS::Heap type is also compatible with C++ containers that properly
  // construct/move/destory elements.
  std::vector<JS::Heap<JS::Value>> container;

  // If we provide a trace method, we can then be used in a JS::Rooted or
  // similar and the GC will be able to successfuly trace us.
  void trace(JSTracer* trc) {
    JS::TraceEdge(trc, &stashed, "stashed value");

    // We can also trace containers as long as we use references.
    for (auto& elem : container) {
      JS::TraceEdge(trc, &elem, "container value");
    }
  }
};

static bool CustomTypeExample(JSContext* cx) {
  // If we use SafeBox as a stack object, then a JS::Rooted is enough.
  JS::Rooted<SafeBox> stackSafe(cx);

  // We can also use js::UniquePtr if SafeBox should be allocated on heap.
  JS::Rooted<js::UniquePtr<SafeBox>> heapSafe(cx, js::MakeUnique<SafeBox>());

  // NOTE: A JS::Rooted<SafeBox*> is a compile error. If one wanted to support
  // rooting bare non-GC pointers then both JS::MapTypeToRootKind and
  // JS::GCPolicy need to be defined for SafeBox*. This should be avoided in
  // favor of using a smart-pointer when possible.

  return true;
}

////////////////////////////////////////////////////////////

// By defining a JS::GCPolicy we can support tracing existing types that we
// cannot add a trace method to such as std::shared_ptr.
//
// In this example, we forward methods to GCPolicy of target type and provide
// reasonable behaviours when we have no current target.
template <typename T>
struct JS::GCPolicy<std::shared_ptr<T>> {
  static void trace(JSTracer* trc, std::shared_ptr<T>* tp, const char* name) {
    if (T* target = tp->get()) {
      GCPolicy<T>::trace(trc, target, name);
    }
  }
  static bool needsSweep(std::shared_ptr<T>* tp) {
    if (T* target = tp->get()) {
      return GCPolicy<T>::needsSweep(target);
    }
    return false;
  }
  static bool isValid(const std::shared_ptr<T>& t) {
    if (T* target = t.get()) {
      return GCPolicy<T>::isValid(*target);
    }
    return true;
  }
};

static bool ExistingTypeExample(JSContext* cx) {
  // We can use std::shared_ptr too since we defined a GCPolicy above.
  JS::Rooted<std::shared_ptr<SafeBox>> sharedSafe(cx,
                                                  std::make_shared<SafeBox>());
  return true;
}

////////////////////////////////////////////////////////////

// When an embedding wishes to keep GC things alive when the JavaScript no
// longer has direct references, it must provide GC roots for the various
// tracing mechanisms to search from. This is done using the PersistentRooted
// type.
//
// Each PersistentRooted will register / unregister with the GC root-list. This
// can be a performance overhead if you rapidly create / destroy C++ objects.
// If you have an array of C++ objects it is preferable to root the container
// rather than putting a PersistentRooted in each element. See the
// SafeBox::container field in the example above.

// A global PersistentRooted is created before SpiderMonkey has initialized so
// we must be careful to not create any JS::Heap fields during construction.
JS::PersistentRooted<js::UniquePtr<SafeBox>> globalPtrSafe;
mozilla::Maybe<JS::PersistentRooted<SafeBox>> globalMaybeSafe;

static bool GlobalRootExample(JSContext* cx) {
  // Initialize the root with cx and allocate a fresh SafeBox.
  globalPtrSafe.init(cx, js::MakeUnique<SafeBox>());

  // If we want to avoid a heap allocation, we can wrap the PersistentRooted in
  // a Maybe. When we emplace the variable, we pass 'cx' for the constructor of
  // PersistentRooted.
  globalMaybeSafe.emplace(cx);

  // IMPORTANT: When using global PersistentRooteds they *must* be cleared
  // before shutdown by calling 'reset()'.
  globalMaybeSafe.reset();
  globalPtrSafe.reset();

  return true;
}

////////////////////////////////////////////////////////////

// Instead of the global variable example above, it is often preferable to
// store PersistentRooted inside embedding data structures. By passing cx to
// the roots during the constructor we can automatically register the roots.
//
// NOTE: The techniques used in the GlobalRootExample with Maybe and UniquePtr
//       can also be applied here.

struct EmbeddingContext {
  JS::PersistentRooted<SafeBox> memberSafe;
  JS::PersistentRooted<JSObject*> memberObjPtr;

  explicit EmbeddingContext(JSContext* cx) : memberSafe(cx), memberObjPtr(cx) {}
};

static bool EmbeddingRootExample(JSContext* cx) {
  js::UniquePtr<EmbeddingContext> ec{new EmbeddingContext(cx)};

  return true;
}

////////////////////////////////////////////////////////////

// The other way around: to store a pointer to a C++ struct from a JS object,
// use a JSClass with a trace hook. Note that this is only required if the C++
// struct can reach a GC pointer.

struct CustomObject {
  enum Slots { OwnedBoxSlot, UnownedBoxSlot, SlotCount };

  // When the CustomObject is collected, delete the stored box.
  static void finalize(JS::GCContext* gcx, JSObject* obj);

  // When a CustomObject is traced, it must trace the stored box.
  static void trace(JSTracer* trc, JSObject* obj);

  static constexpr JSClassOps classOps = {
      // Use .finalize if CustomObject owns the C++ object and should delete it
      // when the CustomObject dies.
      .finalize = finalize,

      // Use .trace whenever the JS object points to a C++ object that can reach
      // other JS objects.
      .trace = trace
  };

  static constexpr JSClass clasp = {
      .name = "Custom",
      .flags =
          JSCLASS_HAS_RESERVED_SLOTS(SlotCount) | JSCLASS_FOREGROUND_FINALIZE,
      .cOps = &classOps};

  static JSObject* create(JSContext* cx, SafeBox* ownedBox1, SafeBox* unownedBox2);

  // Full type of JSObject is not known, so we can't inherit.
  static CustomObject* fromObject(JSObject* obj) {
    return reinterpret_cast<CustomObject*>(obj);
  }
  static JSObject* asObject(CustomObject* custom) {
    return reinterpret_cast<JSObject*>(custom);
  }

  // Retrieve the owned SafeBox* from the reserved slot.
  SafeBox* ownedBox() {
    JSObject* obj = CustomObject::asObject(this);
    return JS::GetMaybePtrFromReservedSlot<SafeBox>(obj, OwnedBoxSlot);
  }

  // Retrieve the unowned SafeBox* from the reserved slot.
  SafeBox* unownedBox() {
    JSObject* obj = CustomObject::asObject(this);
    return JS::GetMaybePtrFromReservedSlot<SafeBox>(obj, UnownedBoxSlot);
  }
};

JSObject* CustomObject::create(JSContext* cx, SafeBox* box1, SafeBox* box2) {
  JS::Rooted<JSObject*> obj(cx, JS_NewObject(cx, &clasp));
  if (!obj) {
    return nullptr;
  }
  JS_SetReservedSlot(obj, OwnedBoxSlot, JS::PrivateValue(box1));
  JS_SetReservedSlot(obj, UnownedBoxSlot, JS::PrivateValue(box2));
  return obj;
}

void CustomObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  delete CustomObject::fromObject(obj)->ownedBox();
  // Do NOT delete unownedBox().
}

void CustomObject::trace(JSTracer* trc, JSObject* obj) {
  // Must trace both boxes, owned or not, in order to keep any referred-to GC
  // things alive.
  SafeBox* b = CustomObject::fromObject(obj)->ownedBox();
  if (b) {
    b->trace(trc);
  }
  b = CustomObject::fromObject(obj)->unownedBox();
  if (b) {
    b->trace(trc);
  }
}

static bool CustomObjectExample(JSContext* cx) {
  JS::RootedObject global(cx, boilerplate::CreateGlobal(cx));
  if (!global) {
    return false;
  }

  JSAutoRealm ar(cx, global);

  SafeBox* box = new SafeBox();
  static SafeBox eternalBox{};
  JSObject* obj = CustomObject::create(cx, box, &eternalBox);
  if (!obj) {
    return false;
  }

  // The CustomObject will be collected, since it hasn't been stored anywhere
  // else, and the SafeBox allocated just above will be destroyed.
  JS_GC(cx);

  return true;
}
////////////////////////////////////////////////////////////

static bool TracingExample(JSContext* cx) {
  if (!CustomTypeExample(cx)) {
    return false;
  }
  if (!ExistingTypeExample(cx)) {
    return false;
  }
  if (!GlobalRootExample(cx)) {
    return false;
  }
  if (!EmbeddingRootExample(cx)) {
    return false;
  }
  if (!CustomObjectExample(cx)) {
    return false;
  }

  return true;
}

int main(int argc, const char* argv[]) {
  if (!boilerplate::RunExample(TracingExample)) {
    return 1;
  }
  return 0;
}
