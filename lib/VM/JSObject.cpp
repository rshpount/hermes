/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "hermes/VM/JSObject.h"

#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/InternalProperty.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSDate.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/StringView.h"

#include "llvm/ADT/SmallSet.h"

namespace hermes {
namespace vm {

ObjectVTable JSObject::vt{
    VTable(CellKind::ObjectKind, sizeof(JSObject)),
    JSObject::_getOwnIndexedRangeImpl,
    JSObject::_haveOwnIndexedImpl,
    JSObject::_getOwnIndexedPropertyFlagsImpl,
    JSObject::_getOwnIndexedImpl,
    JSObject::_setOwnIndexedImpl,
    JSObject::_deleteOwnIndexedImpl,
    JSObject::_checkAllOwnIndexedImpl,
};

// We need a way to generate the names of the direct properties in the data
// segment.
namespace {

/// Add \p N fields to the metadata builder \p mb starting from offset
/// \p props and using the name "@directPropX".
template <int N>
void addDirectPropertyFields(
    const GCHermesValue *props,
    Metadata::Builder &mb) {
  // Make sure the property number fits in a single ASCII digit.
  static_assert(N <= 10, "only up to 10 direct properties are supported");
  static const char propName[] = {'@',
                                  'd',
                                  'i',
                                  'r',
                                  'e',
                                  'c',
                                  't',
                                  'P',
                                  'r',
                                  'o',
                                  'p',
                                  (char)(N - 1 + '0'),
                                  '\0'};
  addDirectPropertyFields<N - 1>(props, mb);
  mb.addField(propName, props + N - 1);
}

template <>
void addDirectPropertyFields<0>(const GCHermesValue *, Metadata::Builder &) {}

} // anonymous namespace.

void ObjectBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const JSObject *>(cell);
  mb.addField("@__proto__", &self->parent_);
  mb.addField("@class", &self->clazz_);
  mb.addField("@propStorage", &self->propStorage_);

  // Declare the direct properties.
  addDirectPropertyFields<JSObject::DIRECT_PROPERTY_SLOTS>(
      self->directProps_, mb);
}

PseudoHandle<JSObject> JSObject::create(
    Runtime *runtime,
    Handle<JSObject> parentHandle) {
  void *mem = runtime->alloc</*fixedSize*/ true>(sizeof(JSObject));
  return createPseudoHandle(new (mem) JSObject(
      runtime,
      &vt.base,
      *parentHandle,
      runtime->getHiddenClassForPrototypeRaw(*parentHandle),
      GCPointerBase::NoBarriers()));
}

PseudoHandle<JSObject> JSObject::create(Runtime *runtime) {
  void *mem = runtime->alloc</*fixedSize*/ true>(sizeof(JSObject));
  JSObject *objProto = runtime->objectPrototypeRawPtr;
  return createPseudoHandle(new (mem) JSObject(
      runtime,
      &vt.base,
      objProto,
      runtime->getHiddenClassForPrototypeRaw(objProto),
      GCPointerBase::NoBarriers()));
}

PseudoHandle<JSObject> JSObject::create(
    Runtime *runtime,
    unsigned propertyCount) {
  void *mem = runtime->alloc</*fixedSize*/ true>(sizeof(JSObject));
  JSObject *objProto = runtime->objectPrototypeRawPtr;
  return runtime->ignoreAllocationFailure(JSObject::allocatePropStorage(
      createPseudoHandle(new (mem) JSObject(
          runtime,
          &vt.base,
          objProto,
          runtime->getHiddenClassForPrototypeRaw(objProto),
          GCPointerBase::NoBarriers())),
      runtime,
      propertyCount));
}

PseudoHandle<JSObject> JSObject::create(
    Runtime *runtime,
    Handle<HiddenClass> clazz) {
  auto obj = JSObject::create(runtime, clazz->getNumProperties());
  obj->clazz_.set(*clazz, &runtime->getHeap());
  // If the hidden class has index like property, we need to clear the fast path
  // flag.
  if (LLVM_UNLIKELY(obj->clazz_->getHasIndexLikeProperties()))
    obj->flags_.fastIndexProperties = false;
  return obj;
}

CallResult<HermesValue> JSObject::createWithException(
    Runtime *runtime,
    Handle<JSObject> parentHandle) {
  return JSObject::create(runtime, parentHandle).getHermesValue();
}

void JSObject::initializeLazyObject(
    Runtime *runtime,
    Handle<JSObject> lazyObject) {
  assert(lazyObject->flags_.lazyObject && "object must be lazy");
  // object is now assumed to be a regular object.
  lazyObject->flags_.lazyObject = 0;

  // only functions can be lazy.
  assert(vmisa<Callable>(lazyObject.get()) && "unexpected lazy object");
  Callable::defineLazyProperties(Handle<Callable>::vmcast(lazyObject), runtime);
}

ObjectID JSObject::getObjectID(JSObject *self, Runtime *runtime) {
  if (LLVM_LIKELY(self->flags_.objectID))
    return self->flags_.objectID;

  // Object ID does not yet exist, get next unique global ID..
  self->flags_.objectID = runtime->generateNextObjectID();
  // Make sure it is not zero.
  if (LLVM_UNLIKELY(!self->flags_.objectID))
    --self->flags_.objectID;
  return self->flags_.objectID;
}

ExecutionStatus
JSObject::setParent(JSObject *self, Runtime *runtime, JSObject *parent) {
  // ES6 9.1.2
  // 4.
  if (self->parent_ == parent)
    return ExecutionStatus::RETURNED;
  // 5.
  if (!self->isExtensible()) {
    return runtime->raiseTypeError("JSObject is not extensible.");
  }
  // 6-8. Check for a prototype cycle.
  for (auto *cur = parent; cur; cur = cur->parent_) {
    if (cur == self)
      return runtime->raiseTypeError("Prototype cycle detected");
  }
  // 9.
  self->parent_.set(parent, &runtime->getHeap());
  // 10.
  return ExecutionStatus::RETURNED;
}

void JSObject::allocateNewSlotStorage(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SlotIndex newSlotIndex,
    Handle<> valueHandle) {
  // If it is a direct property, just store the value and we are done.
  if (LLVM_LIKELY(newSlotIndex < DIRECT_PROPERTY_SLOTS)) {
    selfHandle->directProps_[newSlotIndex].set(
        *valueHandle, &runtime->getHeap());
    return;
  }

  // Make the slot index relative to the indirect storage.
  newSlotIndex -= DIRECT_PROPERTY_SLOTS;

  // Allocate a new property storage if not already allocated.
  if (LLVM_UNLIKELY(!selfHandle->propStorage_)) {
    // Allocate new storage.
    assert(newSlotIndex == 0 && "allocated slot must be at end");
    auto arrRes = runtime->ignoreAllocationFailure(
        PropStorage::create(runtime, DEFAULT_PROPERTY_CAPACITY));
    selfHandle->propStorage_.set(
        vmcast<PropStorage>(arrRes), &runtime->getHeap());
  } else if (LLVM_UNLIKELY(
                 newSlotIndex >= selfHandle->propStorage_->capacity())) {
    // Reallocate the existing one.
    assert(
        newSlotIndex == selfHandle->propStorage_->size() &&
        "allocated slot must be at end");
    auto hnd = runtime->makeMutableHandle(selfHandle->propStorage_);
    PropStorage::resize(hnd, runtime, newSlotIndex + 1);
    selfHandle->propStorage_.set(*hnd, &runtime->getHeap());
  }

  if (newSlotIndex >= selfHandle->propStorage_->size()) {
    assert(
        newSlotIndex == selfHandle->propStorage_->size() &&
        "allocated slot must be at end");
    PropStorage::resizeWithinCapacity(
        selfHandle->propStorage_, runtime, newSlotIndex + 1);
  }
  // If we don't need to resize, just store it directly.
  selfHandle->propStorage_->at(newSlotIndex)
      .set(*valueHandle, &runtime->getHeap());
}

SlotIndex JSObject::addInternalProperty(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    unsigned index,
    Handle<> valueHandle) {
  assert(
      index < InternalProperty::NumInternalProperties &&
      "Internal property index is too large");
  assert(
      !selfHandle->clazz_->isDictionary() &&
      "Internal properties can only be added in class mode");

  auto addResult = HiddenClass::addProperty(
      runtime->makeHandle(selfHandle->clazz_),
      runtime,
      InternalProperty::getSymbolID(index),
      PropertyFlags{});
  assert(
      addResult != ExecutionStatus::EXCEPTION &&
      "Could not possibly grow larger than the limit");
  selfHandle->clazz_.set(*addResult->first, &runtime->getHeap());

  allocateNewSlotStorage(selfHandle, runtime, addResult->second, valueHandle);

  return addResult->second;
}

void JSObject::addInternalProperties(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    unsigned count,
    Handle<> valueHandle) {
  assert(count != 0 && "Cannot add 0 internal properties");
  assert(
      count <= InternalProperty::NumInternalProperties &&
      "Too many internal properties");
  assert(
      !selfHandle->clazz_->isDictionary() &&
      "Internal properties can only be added in class mode");
  assert(
      selfHandle->clazz_->getNumProperties() == 0 &&
      "Internal properties must be added first");
  assert(
      count <= DIRECT_PROPERTY_SLOTS &&
      "We shouldn't add internal properties to indirect storage");

  for (unsigned i = 0; i != count; ++i) {
    auto slotIndex = addInternalProperty(selfHandle, runtime, i, valueHandle);
    (void)slotIndex;
    assert(
        slotIndex == i &&
        "bulk added internal property slot should match its index");
  }
}

CallResult<HermesValue> JSObject::getNamedPropertyValue(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<JSObject> propObj,
    NamedPropertyDescriptor desc) {
  if (LLVM_LIKELY(!desc.flags.accessor))
    return getNamedSlotValue(propObj.get(), desc);

  auto *accessor =
      vmcast<PropertyAccessor>(getNamedSlotValue(propObj.get(), desc));
  if (!accessor->getter)
    return HermesValue::encodeUndefinedValue();

  // Execute the accessor on this object.
  return accessor->getter->executeCall0(
      runtime->makeHandle(accessor->getter), runtime, selfHandle);
}

CallResult<HermesValue> JSObject::getComputedPropertyValue(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<JSObject> propObj,
    ComputedPropertyDescriptor desc) {
  if (LLVM_LIKELY(!desc.flags.accessor))
    return getComputedSlotValue(propObj.get(), runtime, desc);

  auto *accessor = vmcast<PropertyAccessor>(
      getComputedSlotValue(propObj.get(), runtime, desc));
  if (!accessor->getter)
    return HermesValue::encodeUndefinedValue();

  // Execute the accessor on this object.
  return accessor->getter->executeCall0(
      runtime->makeHandle(accessor->getter), runtime, selfHandle);
}

CallResult<Handle<JSArray>> JSObject::getOwnPropertyNames(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    bool onlyEnumerable) {
  if (LLVM_UNLIKELY(selfHandle->flags_.lazyObject)) {
    initializeLazyObject(runtime, selfHandle);
  }

  // Estimate the capacity of the output array.
  auto range = getOwnIndexedRange(selfHandle.get());
  uint32_t capacity =
      selfHandle->clazz_->getNumProperties() + range.second - range.first;

  auto arrayRes = JSArray::create(runtime, capacity, 0);
  if (LLVM_UNLIKELY(arrayRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto array = toHandle(runtime, std::move(*arrayRes));

  // Optional array of SymbolIDs reported via host object API
  llvm::Optional<Handle<JSArray>> hostObjectSymbols;
  size_t hostObjectSymbolCount = 0;

  // If current object is a host object we need to deduplicate its properties
  llvm::SmallSet<SymbolID::RawType, 16> dedupSet;

  // Get host object property names
  if (LLVM_UNLIKELY(selfHandle->flags_.hostObject)) {
    assert(
        range.first == range.second && "Host objects cannot own indexed range");
    auto hostSymbolsRes =
        vmcast<HostObject>(selfHandle.get())->getHostPropertyNames();
    if (hostSymbolsRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if ((hostObjectSymbolCount = (**hostSymbolsRes)->getEndIndex()) != 0) {
      Handle<JSArray> hostSymbols = *hostSymbolsRes;
      hostObjectSymbols = std::move(hostSymbols);
      capacity += hostObjectSymbolCount;
    }
  }

  // Output index.
  uint32_t index = 0;

  // Avoid allocating a new handle per element.
  MutableHandle<> tmpHandle{runtime};

  // Iterate the indexed properties.
  GCScopeMarkerRAII marker{runtime};
  for (auto i = range.first; i != range.second; ++i) {
    auto res = getOwnIndexedPropertyFlags(selfHandle.get(), runtime, i);
    if (!res)
      continue;

    // If specified, check whether it is enumerable.
    if (onlyEnumerable && !res->enumerable)
      continue;

    tmpHandle = HermesValue::encodeDoubleValue(i);
    JSArray::setElementAt(array, runtime, index++, tmpHandle);
    marker.flush();
  }

  // Number of indexed properties.
  uint32_t numIndexed = index;

  // Regular properties with names that are array indexes are stashed here, if
  // encountered.
  llvm::SmallVector<uint32_t, 8> indexNames{};

  // Iterate the named properties excluding those which use Symbols.
  HiddenClass::forEachProperty(
      runtime->makeHandle(selfHandle->clazz_),
      runtime,
      [runtime,
       onlyEnumerable,
       array,
       hostObjectSymbolCount,
       &index,
       &indexNames,
       &tmpHandle,
       &dedupSet](SymbolID id, NamedPropertyDescriptor desc) {
        if (!isPropertyNamePrimitive(id)) {
          return;
        }

        // If specified, check whether it is enumerable.
        if (onlyEnumerable) {
          if (!desc.flags.enumerable)
            return;
        }

        // Host properties might overlap with the ones recognized by the hidden
        // class. If we're dealing with a host object then keep track of hidden
        // class properties for the deduplication purposes.
        if (LLVM_UNLIKELY(hostObjectSymbolCount > 0)) {
          dedupSet.insert(id.unsafeGetRaw());
        }

        // Check if this property is an integer index. If it is, we stash it
        // away to deal with it later. This check should be fast since most
        // property names don't start with a digit.
        auto propNameAsIndex = toArrayIndex(
            runtime->getIdentifierTable().getStringView(runtime, id));
        if (LLVM_UNLIKELY(propNameAsIndex)) {
          indexNames.push_back(*propNameAsIndex);
          return;
        }

        tmpHandle = HermesValue::encodeStringValue(
            runtime->getStringPrimFromSymbolID(id));
        JSArray::setElementAt(array, runtime, index++, tmpHandle);
      });

  // Iterate over HostObject properties and append them to the array. Do not
  // append duplicates.
  if (LLVM_UNLIKELY(hostObjectSymbols)) {
    for (size_t i = 0; i < hostObjectSymbolCount; ++i) {
      assert(
          (*hostObjectSymbols)->at(i).isSymbol() &&
          "Host object needs to return array of SymbolIDs");
      marker.flush();
      SymbolID id = (*hostObjectSymbols)->at(i).getSymbol();
      if (dedupSet.count(id.unsafeGetRaw()) == 0) {
        dedupSet.insert(id.unsafeGetRaw());

        assert(
            !InternalProperty::isInternal(id) &&
            "host object returned reserved symbol");
        auto propNameAsIndex = toArrayIndex(
            runtime->getIdentifierTable().getStringView(runtime, id));
        if (LLVM_UNLIKELY(propNameAsIndex)) {
          indexNames.push_back(*propNameAsIndex);
          continue;
        }
        tmpHandle = HermesValue::encodeStringValue(
            runtime->getStringPrimFromSymbolID(id));
        JSArray::setElementAt(array, runtime, index++, tmpHandle);
      }
    }
  }

  // The end (exclusive) of the named properties.
  uint32_t endNamed = index;

  // Properly set the length of the array.
  auto cr = JSArray::setLength(
      array, runtime, endNamed + indexNames.size(), PropOpFlags{});
  (void)cr;
  assert(
      cr != ExecutionStatus::EXCEPTION && *cr && "JSArray::setLength() failed");

  // If we have no index-like names, we are done.
  if (LLVM_LIKELY(indexNames.empty()))
    return array;

  // In the unlikely event that we encountered index-like names, we need to sort
  // them and merge them with the real indexed properties. Note that it is
  // guaranteed that there are no clashes.
  std::sort(indexNames.begin(), indexNames.end());

  // Also make space for the new elements by shifting all the named properties
  // to the right. First, resize the array.
  JSArray::setStorageEndIndex(array, runtime, endNamed + indexNames.size());

  // Shift the non-index property names. The region [numIndexed..endNamed) is
  // moved to [numIndexed+indexNames.size()..array->size()).
  // TODO: optimize this by implementing memcpy-like functionality in ArrayImpl.
  for (uint32_t last = endNamed, toLast = array->getEndIndex();
       last != numIndexed;) {
    --last;
    --toLast;
    tmpHandle = array->at(last);
    JSArray::setElementAt(array, runtime, toLast, tmpHandle);
  }

  // Now we need to merge the indexes in indexNames and the array
  // [0..numIndexed). We start from the end and copy the larger element from
  // either array.
  // 1+ the destination position to copy into.
  for (uint32_t toLast = numIndexed + indexNames.size(),
                indexNamesLast = indexNames.size();
       toLast != 0;) {
    if (numIndexed) {
      uint32_t a = (uint32_t)array->at(numIndexed - 1).getNumber();
      uint32_t b;

      if (indexNamesLast && (b = indexNames[indexNamesLast - 1]) > a) {
        tmpHandle = HermesValue::encodeDoubleValue(b);
        --indexNamesLast;
      } else {
        tmpHandle = HermesValue::encodeDoubleValue(a);
        --numIndexed;
      }
    } else {
      assert(indexNamesLast && "prematurely ran out of source values");
      tmpHandle =
          HermesValue::encodeDoubleValue(indexNames[indexNamesLast - 1]);
      --indexNamesLast;
    }

    --toLast;
    JSArray::setElementAt(array, runtime, toLast, tmpHandle);
  }

  return array;
}

CallResult<Handle<JSArray>> JSObject::getOwnPropertySymbols(
    Handle<JSObject> selfHandle,
    Runtime *runtime) {
  if (LLVM_UNLIKELY(selfHandle->flags_.lazyObject)) {
    initializeLazyObject(runtime, selfHandle);
  }

  auto arrayRes = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(arrayRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto array = toHandle(runtime, std::move(*arrayRes));

  MutableHandle<SymbolID> tmpHandle{runtime};
  uint32_t index = 0;

  // Iterate the named properties.
  HiddenClass::forEachProperty(
      runtime->makeHandle(selfHandle->clazz_),
      runtime,
      [runtime, &array, &index, &tmpHandle](
          SymbolID id, NamedPropertyDescriptor desc) {
        if (!isSymbolPrimitive(id)) {
          return;
        }

        tmpHandle = id;
        JSArray::setElementAt(array, runtime, index++, tmpHandle);
      });

  // Properly set the length of the array.
  auto cr = JSArray::setLength(array, runtime, index, PropOpFlags());
  (void)cr;
  assert(
      cr != ExecutionStatus::EXCEPTION && *cr && "JSArray::setLength() failed");

  return array;
}

/// Convert a value to string unless already converted
/// \param nameValHandle [Handle<>] the value to convert
/// \param str [MutableHandle<StringPrimitive>] the string is stored
///   there. Must be initialized to null initially.
#define LAZY_TO_STRING(runtime, nameValHandle, str)       \
  do {                                                    \
    if (!str) {                                           \
      auto status = toString_RJS(runtime, nameValHandle); \
      assert(                                             \
          status != ExecutionStatus::EXCEPTION &&         \
          "toString() of primitive cannot fail");         \
      str = status->get();                                \
    }                                                     \
  } while (0)

/// Convert a value to an identifier unless already converted
/// \param nameValHandle [Handle<>] the value to convert
/// \param str [MutableHandle<StringPrimitive>] the string is stored
///   there. Must be initialized to null initially.
/// \param id [SymbolID] the identifier is stored there. Must be initialized
///   to INVALID_IDENTIFIER_ID initially.
#define LAZY_TO_IDENTIFIER(runtime, nameValHandle, str, id)           \
  do {                                                                \
    if (id.isInvalid()) {                                             \
      CallResult<Handle<SymbolID>> idRes{ExecutionStatus::EXCEPTION}; \
      if (str) {                                                      \
        idRes = stringToSymbolID(runtime, str);                       \
      } else {                                                        \
        idRes = valueToSymbolID(runtime, nameValHandle);              \
      }                                                               \
      if (LLVM_UNLIKELY(idRes == ExecutionStatus::EXCEPTION)) {       \
        return ExecutionStatus::EXCEPTION;                            \
      }                                                               \
      id = **idRes;                                                   \
    }                                                                 \
  } while (0)

/// Convert a value to array index, if possible.
/// \param nameValHandle [Handle<>] the value to convert
/// \param str [MutableHandle<StringPrimitive>] the string is stored
///   there. Must be initialized to null initially.
/// \param arrayIndex [OptValue<uint32_t>] the array index is stored
///   there.
#define TO_ARRAY_INDEX(runtime, nameValHandle, str, arrayIndex) \
  do {                                                          \
    arrayIndex = toArrayIndexFastPath(*nameValHandle);          \
    if (!arrayIndex && !nameValHandle->isSymbol()) {            \
      LAZY_TO_STRING(runtime, nameValHandle, str);              \
      arrayIndex = toArrayIndex(runtime, str);                  \
    }                                                           \
  } while (0)

/// \return true if the flags of a new property make it suitable for indexed
///   storage. All new indexed properties are enumerable, writable and
///   configurable and have no accessors.
static bool canNewPropertyBeIndexed(DefinePropertyFlags dpf) {
  return dpf.setEnumerable && dpf.enumerable && dpf.setWritable &&
      dpf.writable && dpf.setConfigurable && dpf.configurable &&
      !dpf.setSetter && !dpf.setGetter;
}

CallResult<bool> JSObject::getOwnComputedPrimitiveDescriptor(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<> nameValHandle,
    ComputedPropertyDescriptor &desc) {
  assert(
      !nameValHandle->isObject() &&
      "nameValHandle passed to "
      "getOwnComputedPrimitiveDescriptor "
      "cannot be an object");

  MutableHandle<StringPrimitive> strPrim{runtime};
  SymbolID id{};

  // Try the fast paths first if we have "fast" index properties and the
  // property name is an obvious index.
  if (selfHandle->flags_.fastIndexProperties) {
    if (auto arrayIndex = toArrayIndexFastPath(*nameValHandle)) {
      auto res =
          getOwnIndexedPropertyFlags(selfHandle.get(), runtime, *arrayIndex);
      if (res) {
        // This a valid array index, residing in our indexed storage.
        desc.flags = *res;
        desc.flags.indexed = 1;
        desc.slot = *arrayIndex;
        return true;
      }

      // This a valid array index, but we don't have it in our indexed storage,
      // and we don't have index-like named properties.
      return false;
    }
  }

  // Convert the string to an SymbolID;
  LAZY_TO_IDENTIFIER(runtime, nameValHandle, strPrim, id);

  // Look for a named property with this name.
  if (JSObject::getOwnNamedDescriptor(
          selfHandle, runtime, id, desc.castToNamedPropertyDescriptorRef())) {
    return true;
  }

  // If we have indexed storage, perform potentially expensive conversions
  // to array index and check it.
  if (selfHandle->flags_.indexedStorage) {
    // If the name is a valid integer array index, store it here.
    OptValue<uint32_t> arrayIndex;

    // Try to convert the property name to an array index.
    TO_ARRAY_INDEX(runtime, nameValHandle, strPrim, arrayIndex);

    if (arrayIndex) {
      auto res =
          getOwnIndexedPropertyFlags(selfHandle.get(), runtime, *arrayIndex);
      if (res) {
        desc.flags = *res;
        desc.flags.indexed = 1;
        desc.slot = *arrayIndex;
        return true;
      }
    }
  }

  if (LLVM_UNLIKELY(selfHandle->flags_.lazyObject)) {
    JSObject::initializeLazyObject(runtime, selfHandle);
    return getOwnComputedPrimitiveDescriptor(
        selfHandle, runtime, nameValHandle, desc);
  }
  return false;
}

CallResult<bool> JSObject::getOwnComputedDescriptor(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<> nameValHandle,
    ComputedPropertyDescriptor &desc) {
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return JSObject::getOwnComputedPrimitiveDescriptor(
      selfHandle, runtime, *converted, desc);
}

JSObject *JSObject::getNamedDescriptor(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    PropertyFlags expectedFlags,
    NamedPropertyDescriptor &desc) {
  if (findProperty(selfHandle, runtime, name, expectedFlags, desc))
    return *selfHandle;

  // Check here for host object flag.  This means that "normal" own
  // properties above win over host-defined properties, but there's no
  // cost imposed on own property lookups.  This should do what we
  // need in practice, and we can define host vs js property
  // disambiguation however we want.  This is here in order to avoid
  // impacting perf for the common case where an own property exists
  // in normal storage.
  if (LLVM_UNLIKELY(selfHandle->flags_.hostObject)) {
    desc.flags.hostObject = true;
    desc.flags.writable = true;
    return *selfHandle;
  }

  if (LLVM_UNLIKELY(selfHandle->flags_.lazyObject)) {
    // Initialize the object and perform the lookup again.
    JSObject::initializeLazyObject(runtime, selfHandle);

    if (findProperty(selfHandle, runtime, name, expectedFlags, desc))
      return *selfHandle;
  }
  if (selfHandle->parent_) {
    MutableHandle<JSObject> mutableSelfHandle{runtime, selfHandle->parent_};

    do {
      // Check the most common case first, at the cost of some code duplication.
      if (LLVM_LIKELY(
              !mutableSelfHandle->flags_.lazyObject &&
              !mutableSelfHandle->flags_.hostObject)) {
      findProp:
        if (findProperty(
                mutableSelfHandle,
                runtime,
                name,
                PropertyFlags::invalid(),
                desc)) {
          return *mutableSelfHandle;
        }
      } else if (LLVM_UNLIKELY(mutableSelfHandle->flags_.lazyObject)) {
        JSObject::initializeLazyObject(runtime, mutableSelfHandle);
        goto findProp;
      } else {
        assert(
            mutableSelfHandle->flags_.hostObject &&
            "descriptor flags are impossible");
        desc.flags.hostObject = true;
        desc.flags.writable = true;
        return *mutableSelfHandle;
      }
    } while ((mutableSelfHandle = mutableSelfHandle->parent_));
  }

  return nullptr;
}

ExecutionStatus JSObject::getComputedPrimitiveDescriptor(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<> nameValHandle,
    MutableHandle<JSObject> &propObj,
    ComputedPropertyDescriptor &desc) {
  assert(
      !nameValHandle->isObject() &&
      "nameValHandle passed to "
      "getComputedPrimitiveDescriptor cannot "
      "be an object");

  propObj = selfHandle.get();

  GCScopeMarkerRAII marker{runtime};
  do {
    auto cr = getOwnComputedPrimitiveDescriptor(
        propObj, runtime, nameValHandle, desc);
    if (cr == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if (*cr) {
      return ExecutionStatus::RETURNED;
    }

    if (LLVM_UNLIKELY(propObj->flags_.hostObject)) {
      desc.flags.hostObject = true;
      desc.flags.writable = true;
      return ExecutionStatus::RETURNED;
    }
    // Flush at the end of the loop to allow first iteration to be as fast as
    // possible.
    marker.flush();
  } while ((propObj = propObj->parent_));
  return ExecutionStatus::RETURNED;
}

ExecutionStatus JSObject::getComputedDescriptor(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<> nameValHandle,
    MutableHandle<JSObject> &propObj,
    ComputedPropertyDescriptor &desc) {
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  getComputedPrimitiveDescriptor(
      selfHandle, runtime, *converted, propObj, desc);
  return ExecutionStatus::RETURNED;
}

CallResult<HermesValue> JSObject::getNamed_RJS(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    PropOpFlags opFlags,
    PropertyCacheEntry *cacheEntry) {
  NamedPropertyDescriptor desc;

  // Locate the descriptor. propObj contains the object which may be anywhere
  // along the prototype chain.
  JSObject *propObj = getNamedDescriptor(selfHandle, runtime, name, desc);
  if (!propObj) {
    if (LLVM_UNLIKELY(opFlags.getMustExist())) {
      return runtime->raiseReferenceError(
          TwineChar16("Property '") +
          runtime->getIdentifierTable().getStringView(runtime, name) +
          "' doesn't exist");
    }
    return HermesValue::encodeUndefinedValue();
  }

  if (LLVM_LIKELY(!desc.flags.accessor && !desc.flags.hostObject)) {
    // Populate the cache if requested.
    if (cacheEntry && !propObj->getClass()->isDictionary()) {
      cacheEntry->clazz = propObj->getClass();
      cacheEntry->slot = desc.slot;
    }
    return getNamedSlotValue(propObj, desc);
  }

  if (desc.flags.accessor) {
    auto *accessor = vmcast<PropertyAccessor>(getNamedSlotValue(propObj, desc));
    if (!accessor->getter)
      return HermesValue::encodeUndefinedValue();

    // Execute the accessor on this object.
    return Callable::executeCall0(
        runtime->makeHandle(accessor->getter), runtime, selfHandle);
  } else {
    assert(desc.flags.hostObject && "descriptor flags are impossible");
    return vmcast<HostObject>(propObj)->get(name);
  }
}

CallResult<HermesValue> JSObject::getNamedOrIndexed(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    PropOpFlags opFlags) {
  if (LLVM_UNLIKELY(selfHandle->flags_.indexedStorage)) {
    // Note that getStringView can be satisfied without materializing the
    // Identifier.
    const auto strView =
        runtime->getIdentifierTable().getStringView(runtime, name);
    if (auto nameAsIndex = toArrayIndex(strView)) {
      return getComputed_RJS(
          selfHandle,
          runtime,
          runtime->makeHandle(HermesValue::encodeNumberValue(*nameAsIndex)));
    }
    // Here we have indexed properties but the symbol was not index-like.
    // Fall through to getNamed().
  }
  return getNamed_RJS(selfHandle, runtime, name, opFlags);
}

CallResult<HermesValue> JSObject::getComputed_RJS(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<> nameValHandle) {
  // Try the fast-path first: no "index-like" properties and the "name" already
  // is a valid integer index.
  if (selfHandle->flags_.fastIndexProperties) {
    if (auto arrayIndex = toArrayIndexFastPath(*nameValHandle)) {
      // Do we have this value present in our array storage? If so, return it.
      HermesValue ourValue =
          getOwnIndexed(selfHandle.get(), runtime, *arrayIndex);
      if (LLVM_LIKELY(!ourValue.isEmpty()))
        return ourValue;
    }
  }

  // If nameValHandle is an object, we should convert it to string now,
  // because toString may have side-effect, and we want to do this only
  // once.
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto nameValPrimitiveHandle = *converted;

  ComputedPropertyDescriptor desc;

  // Locate the descriptor. propObj contains the object which may be anywhere
  // along the prototype chain.
  MutableHandle<JSObject> propObj{runtime};
  getComputedPrimitiveDescriptor(
      selfHandle, runtime, nameValPrimitiveHandle, propObj, desc);

  if (!propObj)
    return HermesValue::encodeUndefinedValue();

  if (LLVM_LIKELY(!desc.flags.accessor && !desc.flags.hostObject))
    return getComputedSlotValue(propObj.get(), runtime, desc);

  if (desc.flags.accessor) {
    auto *accessor = vmcast<PropertyAccessor>(
        getComputedSlotValue(propObj.get(), runtime, desc));
    if (!accessor->getter)
      return HermesValue::encodeUndefinedValue();

    // Execute the accessor on this object.
    return accessor->getter->executeCall0(
        runtime->makeHandle(accessor->getter), runtime, selfHandle);
  } else {
    assert(desc.flags.hostObject && "descriptor flags are impossible");
    MutableHandle<StringPrimitive> strPrim{runtime};
    SymbolID id{};
    LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, strPrim, id);
    auto propRes = vmcast<HostObject>(selfHandle.get())->get(id);
    if (propRes == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    return propRes;
  }
}

bool JSObject::hasNamed(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name) {
  NamedPropertyDescriptor desc;
  JSObject *propObj = getNamedDescriptor(selfHandle, runtime, name, desc);
  return propObj ? true : false;
}

bool JSObject::hasNamedOrIndexed(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name) {
  if (LLVM_UNLIKELY(selfHandle->flags_.indexedStorage)) {
    const auto strView =
        runtime->getIdentifierTable().getStringView(runtime, name);
    if (auto nameAsIndex = toArrayIndex(strView)) {
      if (haveOwnIndexed(selfHandle.get(), runtime, *nameAsIndex)) {
        return true;
      }
      if (selfHandle->flags_.fastIndexProperties) {
        return false;
      }
    }
    // Here we have indexed properties but the symbol was not stored in the
    // indexedStorage.
    // Fall through to getNamed().
  }
  return hasNamed(selfHandle, runtime, name);
}

CallResult<bool> JSObject::hasComputed(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<> nameValHandle) {
  // Try the fast-path first: no "index-like" properties and the "name" already
  // is a valid integer index.
  if (selfHandle->flags_.fastIndexProperties) {
    if (auto arrayIndex = toArrayIndexFastPath(*nameValHandle)) {
      // Do we have this value present in our array storage? If so, return true.
      if (haveOwnIndexed(selfHandle.get(), runtime, *arrayIndex)) {
        return true;
      }
    }
  }

  ComputedPropertyDescriptor desc;
  MutableHandle<JSObject> propObj{runtime};
  if (getComputedDescriptor(
          selfHandle, runtime, nameValHandle, propObj, desc) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return !!propObj;
}

static ExecutionStatus raiseErrorForOverridingStaticBuiltin(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<SymbolID> name) {
  Handle<StringPrimitive> methodNameHnd =
      runtime->makeHandle(runtime->getStringPrimFromSymbolID(name.get()));
  // If the 'name' property does not exist or is an accessor, we don't display
  // the name.
  NamedPropertyDescriptor desc;
  auto *obj = JSObject::getNamedDescriptor(
      selfHandle, runtime, Predefined::getSymbolID(Predefined::name), desc);
  if (!obj || desc.flags.accessor) {
    return runtime->raiseTypeError(
        TwineChar16("Attempting to override read-only builtin method '") +
        TwineChar16(methodNameHnd.get()) + "'");
  }

  // Display the name property of the builtin object if it is a string.
  StringPrimitive *objName = dyn_vmcast<StringPrimitive>(
      JSObject::getNamedSlotValue(selfHandle.get(), desc));
  if (!objName) {
    return runtime->raiseTypeError(
        TwineChar16("Attempting to override read-only builtin method '") +
        TwineChar16(methodNameHnd.get()) + "'");
  }

  return runtime->raiseTypeError(
      TwineChar16("Attempting to override read-only builtin method '") +
      TwineChar16(objName) + "." + TwineChar16(methodNameHnd.get()) + "'");
}

CallResult<bool> JSObject::putNamed_RJS(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    Handle<> valueHandle,
    PropOpFlags opFlags) {
  NamedPropertyDescriptor desc;

  // Look for the property in this object or along the prototype chain.
  JSObject *propObj = getNamedDescriptor(
      selfHandle,
      runtime,
      name,
      PropertyFlags::defaultNewNamedPropertyFlags(),
      desc);

  // If the property exists.
  if (propObj) {
    if (LLVM_UNLIKELY(desc.flags.accessor)) {
      auto *accessor =
          vmcast<PropertyAccessor>(getNamedSlotValue(propObj, desc));

      // If it is a read-only accessor, fail.
      if (!accessor->setter) {
        if (opFlags.getThrowOnError()) {
          return runtime->raiseTypeError(
              TwineChar16("Cannot assign to read-only property '") +
              runtime->getIdentifierTable().getStringView(runtime, name) + "'");
        }
        return false;
      }

      // Execute the accessor on this object.
      if (accessor->setter->executeCall1(
              runtime->makeHandle(accessor->setter),
              runtime,
              selfHandle,
              *valueHandle) == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      return true;
    }

    if (LLVM_UNLIKELY(!desc.flags.writable)) {
      if (desc.flags.staticBuiltin) {
#ifdef NDEBUG
        // TODO(T35544739): clean up the experiment after we are done.
        auto experimentFlags = runtime->getVMExperimentFlags();
        if (experimentFlags & experiments::FreezeBuiltinsAndFatalOnOverride) {
          hermes_fatal("Attempting to override a static builtin.");
        } else {
          return raiseErrorForOverridingStaticBuiltin(
              selfHandle, runtime, runtime->makeHandle(name));
        }
#else
        return raiseErrorForOverridingStaticBuiltin(
            selfHandle, runtime, runtime->makeHandle(name));
#endif
      }
      if (opFlags.getThrowOnError()) {
        return runtime->raiseTypeError(
            TwineChar16("Cannot assign to read-only property '") +
            runtime->getIdentifierTable().getStringView(runtime, name) + "'");
      }
      return false;
    }

    // If it is a property in this object.
    if (propObj == *selfHandle) {
      if (LLVM_LIKELY(!desc.flags.internalSetter && !desc.flags.hostObject)) {
        setNamedSlotValue(*selfHandle, runtime, desc, *valueHandle);
        return true;
      }
      if (desc.flags.internalSetter) {
        // NOTE: this check slows down property writes up to 3%, because even
        // though it is predicted as not-taken, it occurs on every single
        // property write. Combining it with the accessor check above
        // (LLVM_UNLIKELY(desc.flags.accessor || desc.flags.internalSetter))
        // and moving the other checks in the accessor branch, brings the
        // slow-down to about 2%. (Similarly in setNamedPropertyValue()).
        return internalSetter(
            selfHandle, runtime, name, desc, valueHandle, opFlags);
      } else {
        assert(desc.flags.hostObject && "descriptor flags are impossible");
        return vmcast<HostObject>(selfHandle.get())->set(name, *valueHandle);
      }
    }
  }

  // The property doesn't exist in this object.

  // Does the caller require it to exist?
  if (LLVM_UNLIKELY(opFlags.getMustExist())) {
    return runtime->raiseReferenceError(
        TwineChar16("Property '") +
        runtime->getIdentifierTable().getStringView(runtime, name) +
        "' doesn't exist");
  }

  // Add a new property.

  return addOwnProperty(
      selfHandle,
      runtime,
      name,
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      valueHandle,
      opFlags);
}

CallResult<bool> JSObject::putNamedOrIndexed(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    Handle<> valueHandle,
    PropOpFlags opFlags) {
  if (LLVM_UNLIKELY(selfHandle->flags_.indexedStorage)) {
    // Note that getStringView can be satisfied without materializing the
    // Identifier.
    const auto strView =
        runtime->getIdentifierTable().getStringView(runtime, name);
    if (auto nameAsIndex = toArrayIndex(strView)) {
      return putComputed_RJS(
          selfHandle,
          runtime,
          runtime->makeHandle(HermesValue::encodeNumberValue(*nameAsIndex)),
          valueHandle,
          opFlags);
    }
    // Here we have indexed properties but the symbol was not index-like.
    // Fall through to putNamed().
  }
  return putNamed_RJS(selfHandle, runtime, name, valueHandle, opFlags);
}

CallResult<bool> JSObject::putComputed_RJS(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<> nameValHandle,
    Handle<> valueHandle,
    PropOpFlags opFlags) {
  assert(
      !opFlags.getMustExist() &&
      "mustExist flag cannot be used with computed properties");

  // Try the fast-path first: no "index-like" properties, the "name" already
  // is a valid integer index, and it is present in storage.
  if (selfHandle->flags_.fastIndexProperties) {
    if (auto arrayIndex = toArrayIndexFastPath(*nameValHandle)) {
      if (haveOwnIndexed(selfHandle.get(), runtime, *arrayIndex)) {
        auto result =
            setOwnIndexed(selfHandle, runtime, *arrayIndex, valueHandle);
        if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
        if (LLVM_LIKELY(*result))
          return true;
        if (opFlags.getThrowOnError()) {
          // TODO: better message.
          return runtime->raiseTypeError("Cannot assign to read-only property");
        }
        return false;
      }
    }
  }

  // If nameValHandle is an object, we should convert it to string now,
  // because toString may have side-effect, and we want to do this only
  // once.
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto nameValPrimitiveHandle = *converted;

  ComputedPropertyDescriptor desc;

  // Look for the property in this object or along the prototype chain.
  MutableHandle<JSObject> propObj{runtime};
  getComputedPrimitiveDescriptor(
      selfHandle, runtime, nameValPrimitiveHandle, propObj, desc);

  // If the property exists.
  if (propObj) {
    // Is it an accessor?
    if (LLVM_UNLIKELY(desc.flags.accessor)) {
      auto *accessor = vmcast<PropertyAccessor>(
          getComputedSlotValue(propObj.get(), runtime, desc));

      // If it is a read-only accessor, fail.
      if (!accessor->setter) {
        if (opFlags.getThrowOnError()) {
          // TODO: better message.
          return runtime->raiseTypeError("Cannot assign to read-only property");
        }
        return false;
      }

      // Execute the accessor on this object.
      if (accessor->setter->executeCall1(
              runtime->makeHandle(accessor->setter),
              runtime,
              selfHandle,
              valueHandle.get()) == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      return true;
    }

    if (LLVM_UNLIKELY(!desc.flags.writable)) {
      if (desc.flags.staticBuiltin) {
#ifdef NDEBUG
        // TODO(T35544739): clean up the experiment after we are done.
        auto experimentFlags = runtime->getVMExperimentFlags();
        if (experimentFlags & experiments::FreezeBuiltinsAndFatalOnOverride) {
          hermes_fatal("Attempting to override a static builtin.");
        } else {
          MutableHandle<StringPrimitive> strPrim{runtime};
          SymbolID id{};
          LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, strPrim, id);
          return raiseErrorForOverridingStaticBuiltin(
              selfHandle, runtime, runtime->makeHandle(id));
        }
#else
        MutableHandle<StringPrimitive> strPrim{runtime};
        SymbolID id{};
        LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, strPrim, id);
        return raiseErrorForOverridingStaticBuiltin(
            selfHandle, runtime, runtime->makeHandle(id));
#endif
      }
      if (opFlags.getThrowOnError()) {
        // TODO: better message.
        return runtime->raiseTypeError("Cannot assign to read-only property");
      }
      return false;
    }

    // If it is a property in this object.
    if (propObj == selfHandle) {
      if (LLVM_UNLIKELY(desc.flags.hostObject)) {
        MutableHandle<StringPrimitive> strPrim{runtime};
        SymbolID id{};
        LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, strPrim, id);
        return vmcast<HostObject>(selfHandle.get())->set(id, *valueHandle);
      }
      if (LLVM_UNLIKELY(
              setComputedSlotValue(selfHandle, runtime, desc, valueHandle) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return true;
    }
  }

  // A named property doesn't exist in this object.

  /// Can we add more properties?
  if (!selfHandle->isExtensible()) {
    if (opFlags.getThrowOnError()) {
      return runtime->raiseTypeError(
          "cannot add a new property"); // TODO: better message.
    }
    return false;
  }

  MutableHandle<StringPrimitive> strPrim{runtime};
  SymbolID id{};

  // If we have indexed storage we must check whether the property is an index,
  // and if it is, store it in indexed storage.
  if (selfHandle->flags_.indexedStorage) {
    OptValue<uint32_t> arrayIndex;
    TO_ARRAY_INDEX(runtime, nameValPrimitiveHandle, strPrim, arrayIndex);
    if (arrayIndex) {
      // Check whether we need to update array's ".length" property.
      if (auto *array = dyn_vmcast<JSArray>(selfHandle.get())) {
        if (LLVM_UNLIKELY(*arrayIndex >= JSArray::getLength(array))) {
          auto cr = putNamed_RJS(
              selfHandle,
              runtime,
              Predefined::getSymbolID(Predefined::length),
              runtime->makeHandle(
                  HermesValue::encodeNumberValue(*arrayIndex + 1)),
              opFlags);
          if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
            return ExecutionStatus::EXCEPTION;
          if (LLVM_UNLIKELY(!*cr))
            return false;
        }
      }

      auto result =
          setOwnIndexed(selfHandle, runtime, *arrayIndex, valueHandle);
      if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      if (LLVM_LIKELY(*result))
        return true;

      if (opFlags.getThrowOnError()) {
        // TODO: better message.
        return runtime->raiseTypeError("Cannot assign to read-only property");
      }
      return false;
    }
  }

  LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, strPrim, id);

  // Add a new named property.
  return addOwnProperty(
      selfHandle,
      runtime,
      id,
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      valueHandle,
      opFlags);
}

CallResult<bool> JSObject::deleteNamed(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    PropOpFlags opFlags) {
  assert(
      !opFlags.getMustExist() && "mustExist cannot be specified when deleting");

  // Find the property by name.
  NamedPropertyDescriptor desc;
  auto pos = findProperty(selfHandle, runtime, name, desc);

  // If the property doesn't exist in this object, return success.
  if (!pos) {
    if (LLVM_UNLIKELY(selfHandle->flags_.lazyObject)) {
      // object is lazy, initialize and read again.
      initializeLazyObject(runtime, selfHandle);
      pos = findProperty(selfHandle, runtime, name, desc);
      if (!pos) // still not there, return true.
        return true;
    } else {
      return true;
    }
  }
  // If the property isn't configurable, fail.
  if (LLVM_UNLIKELY(!desc.flags.configurable)) {
    if (opFlags.getThrowOnError()) {
      return runtime->raiseTypeError(
          TwineChar16("Property '") +
          runtime->getIdentifierTable().getStringView(runtime, name) +
          "' is not configurable");
    }
    return false;
  }

  // Clear the deleted property value to prevent memory leaks.
  setNamedSlotValue(
      *selfHandle, runtime, desc, HermesValue::encodeEmptyValue());

  // Perform the actual deletion.
  auto newClazz = HiddenClass::deleteProperty(
      runtime->makeHandle(selfHandle->clazz_), runtime, *pos);
  selfHandle->clazz_.set(*newClazz, &runtime->getHeap());

  return true;
}

CallResult<bool> JSObject::deleteComputed(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<> nameValHandle,
    PropOpFlags opFlags) {
  assert(
      !opFlags.getMustExist() && "mustExist cannot be specified when deleting");

  // If nameValHandle is an object, we should convert it to string now,
  // because toString may have side-effect, and we want to do this only
  // once.
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto nameValPrimitiveHandle = *converted;

  MutableHandle<StringPrimitive> strPrim{runtime};
  SymbolID id;
  // If the name is a valid integer array index, store it here.
  OptValue<uint32_t> arrayIndex;

  // If we have indexed storage, we must attempt to convert the name to array
  // index, even if the conversion is expensive.
  if (selfHandle->flags_.indexedStorage)
    TO_ARRAY_INDEX(runtime, nameValPrimitiveHandle, strPrim, arrayIndex);

  // Try the fast-path first: the "name" is a valid array index and we don't
  // have "index-like" named properties.
  if (arrayIndex && selfHandle->flags_.fastIndexProperties) {
    // Delete the indexed property.
    if (deleteOwnIndexed(selfHandle, runtime, *arrayIndex))
      return true;

    // Cannot delete property (for example this may be a typed array).
    if (opFlags.getThrowOnError()) {
      // TODO: better error message.
      return runtime->raiseTypeError("Cannot delete property");
    }
    return false;
  }

  // slow path, check if object is lazy before continuing.
  if (LLVM_UNLIKELY(selfHandle->flags_.lazyObject)) {
    // initialize and try again.
    initializeLazyObject(runtime, selfHandle);
    return deleteComputed(selfHandle, runtime, nameValHandle, opFlags);
  }

  // Convert the string to an SymbolID;
  LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, strPrim, id);

  // Find the property by name.
  NamedPropertyDescriptor desc;
  auto pos = findProperty(selfHandle, runtime, id, desc);

  // If the property exists, make sure it is configurable.
  if (pos) {
    // If the property isn't configurable, fail.
    if (LLVM_UNLIKELY(!desc.flags.configurable)) {
      if (opFlags.getThrowOnError()) {
        // TODO: a better message.
        return runtime->raiseTypeError("Property is not configurable");
      }
      return false;
    }
  }

  // At this point we know that the named property either doesn't exist, or
  // is configurable and so can be deleted.

  // If it is an "index-like" property, we must also delete the "shadow" indexed
  // property in order to keep Array.length correct.
  if (arrayIndex) {
    if (!deleteOwnIndexed(selfHandle, runtime, *arrayIndex)) {
      // Cannot delete property (for example this may be a typed array).
      if (opFlags.getThrowOnError()) {
        // TODO: better error message.
        return runtime->raiseTypeError("Cannot delete property");
      }
      return false;
    }
  }

  // Finally delete the named property (if it exists).
  if (pos) {
    // Clear the deleted property value to prevent memory leaks.
    setNamedSlotValue(
        *selfHandle, runtime, desc, HermesValue::encodeEmptyValue());

    // Remove the property descriptor.
    auto newClazz = HiddenClass::deleteProperty(
        runtime->makeHandle(selfHandle->clazz_), runtime, *pos);
    selfHandle->clazz_.set(*newClazz, &runtime->getHeap());
  }
  return true;
}

CallResult<bool> JSObject::defineOwnProperty(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  assert(
      !opFlags.getMustExist() && "cannot use mustExist with defineOwnProperty");
  assert(
      !(dpFlags.setValue && dpFlags.isAccessor()) &&
      "Cannot set both value and accessor");
  assert(
      (dpFlags.setValue || dpFlags.isAccessor() ||
       valueOrAccessor.get().isUndefined()) &&
      "value must be undefined when all of setValue/setSetter/setGetter are "
      "false");
#ifndef NDEBUG
  if (dpFlags.isAccessor()) {
    assert(valueOrAccessor.get().isPointer() && "accessor must be non-empty");
    assert(
        !dpFlags.setWritable && !dpFlags.writable &&
        "writable must not be set with accessors");
  }
#endif

  // Is it an existing property.
  NamedPropertyDescriptor desc;
  auto pos = findProperty(selfHandle, runtime, name, desc);
  if (pos) {
    return updateOwnProperty(
        selfHandle,
        runtime,
        name,
        *pos,
        desc,
        dpFlags,
        valueOrAccessor,
        opFlags);
  }

  // if the property was not found and the object is lazy we need to initialize
  // it and try again.
  if (LLVM_UNLIKELY(selfHandle->flags_.lazyObject)) {
    JSObject::initializeLazyObject(runtime, selfHandle);
    return defineOwnProperty(
        selfHandle, runtime, name, dpFlags, valueOrAccessor, opFlags);
  }

  return addOwnProperty(
      selfHandle, runtime, name, dpFlags, valueOrAccessor, opFlags);
}

ExecutionStatus JSObject::defineNewOwnProperty(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    PropertyFlags propertyFlags,
    Handle<> valueOrAccessor) {
  assert(
      !(propertyFlags.accessor && !valueOrAccessor.get().isPointer()) &&
      "accessor must be non-empty");
  assert(
      !(propertyFlags.accessor && propertyFlags.writable) &&
      "writable must not be set with accessors");
  assert(
      !HiddenClass::debugIsPropertyDefined(selfHandle->clazz_, name) &&
      "new property is already defined");

  return addOwnPropertyImpl(
      selfHandle, runtime, name, propertyFlags, valueOrAccessor);
}

CallResult<bool> JSObject::defineOwnComputedPrimitive(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<> nameValHandle,
    DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  assert(
      !nameValHandle->isObject() &&
      "nameValHandle passed to "
      "defineOwnComputedPrimitive() cannot be "
      "an object");
  assert(
      !opFlags.getMustExist() && "cannot use mustExist with defineOwnProperty");
  assert(
      !(dpFlags.setValue && dpFlags.isAccessor()) &&
      "Cannot set both value and accessor");
  assert(
      (dpFlags.setValue || dpFlags.isAccessor() ||
       valueOrAccessor.get().isUndefined()) &&
      "value must be undefined when all of setValue/setSetter/setGetter are "
      "false");
  assert(
      !dpFlags.enableInternalSetter &&
      "Cannot set internalSetter on a computed property");
#ifndef NDEBUG
  if (dpFlags.isAccessor()) {
    assert(valueOrAccessor.get().isPointer() && "accessor must be non-empty");
    assert(
        !dpFlags.setWritable && !dpFlags.writable &&
        "writable must not be set with accessors");
  }
#endif

  MutableHandle<StringPrimitive> strPrim{runtime};
  SymbolID id{};
  // If the name is a valid integer array index, store it here.
  OptValue<uint32_t> arrayIndex;

  // If we have indexed storage, we must attempt to convert the name to array
  // index, even if the conversion is expensive.
  if (selfHandle->flags_.indexedStorage)
    TO_ARRAY_INDEX(runtime, nameValHandle, strPrim, arrayIndex);

  // If not storing a property with an array index name, or if we don't have
  // indexed storage, just pass to the named routine.
  if (!arrayIndex) {
    LAZY_TO_IDENTIFIER(runtime, nameValHandle, strPrim, id);
    return defineOwnProperty(
        selfHandle, runtime, id, dpFlags, valueOrAccessor, opFlags);
  }

  // At this point we know that we have indexed storage and that the property
  // has an index-like name.

  // First check if a named property with the same name exists.
  if (selfHandle->clazz_->getHasIndexLikeProperties()) {
    LAZY_TO_IDENTIFIER(runtime, nameValHandle, strPrim, id);

    NamedPropertyDescriptor desc;
    auto pos = findProperty(selfHandle, runtime, id, desc);
    // If we found a named property, update it.
    if (pos) {
      return updateOwnProperty(
          selfHandle,
          runtime,
          id,
          *pos,
          desc,
          dpFlags,
          valueOrAccessor,
          opFlags);
    }
  }

  // Does an indexed property with that index exist?
  auto indexedPropPresent =
      getOwnIndexedPropertyFlags(selfHandle.get(), runtime, *arrayIndex);
  if (indexedPropPresent) {
    // The current value of the property.
    HermesValue curValueOrAccessor =
        getOwnIndexed(selfHandle.get(), runtime, *arrayIndex);

    auto updateStatus = checkPropertyUpdate(
        runtime,
        *indexedPropPresent,
        dpFlags,
        curValueOrAccessor,
        valueOrAccessor,
        opFlags);
    if (updateStatus == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    if (updateStatus->first == PropertyUpdateStatus::failed)
      return false;

    // The property update is valid, but can the property remain an "indexed"
    // property, or do we need to convert it to a named property?
    // If the property flags didn't change, the property remains indexed.
    if (updateStatus->second == *indexedPropPresent) {
      // If the value doesn't change, we are done.
      if (updateStatus->first == PropertyUpdateStatus::done)
        return true;

      // If we successfully updated the value, we are done.
      auto result =
          setOwnIndexed(selfHandle, runtime, *arrayIndex, valueOrAccessor);
      if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      if (*result)
        return true;

      if (opFlags.getThrowOnError()) {
        // TODO: better error message.
        return runtime->raiseTypeError(
            "cannot change read-only property value");
      }

      return false;
    }

    // OK, we need to convert an indexed property to a named one.

    // Check whether to use the supplied value, or to reuse the old one, as we
    // are simply reconfiguring it.
    MutableHandle<> value{runtime};
    if (dpFlags.setValue || dpFlags.isAccessor()) {
      value = valueOrAccessor.get();
    } else {
      value = curValueOrAccessor;
    }

    // Update dpFlags to match the existing property flags.
    dpFlags.setEnumerable = 1;
    dpFlags.setWritable = 1;
    dpFlags.setConfigurable = 1;
    dpFlags.enumerable = updateStatus->second.enumerable;
    dpFlags.writable = updateStatus->second.writable;
    dpFlags.configurable = updateStatus->second.configurable;

    // Delete the existing indexed property.
    if (!deleteOwnIndexed(selfHandle, runtime, *arrayIndex)) {
      if (opFlags.getThrowOnError()) {
        // TODO: better error message.
        return runtime->raiseTypeError("Cannot define property");
      }
      return false;
    }

    // Add the new named property.
    LAZY_TO_IDENTIFIER(runtime, nameValHandle, strPrim, id);
    return addOwnProperty(selfHandle, runtime, id, dpFlags, value, opFlags);
  }

  /// Can we add new properties?
  if (!selfHandle->isExtensible()) {
    if (opFlags.getThrowOnError()) {
      return runtime->raiseTypeError(
          "cannot add a new property"); // TODO: better message.
    }
    return false;
  }

  // This is a new property with an index-like name.
  // Check whether we need to update array's ".length" property.
  bool updateLength = false;
  if (auto arrayHandle = Handle<JSArray>::dyn_vmcast(runtime, selfHandle)) {
    if (LLVM_UNLIKELY(*arrayIndex >= JSArray::getLength(*arrayHandle))) {
      NamedPropertyDescriptor lengthDesc;
      bool lengthPresent = getOwnNamedDescriptor(
          arrayHandle,
          runtime,
          Predefined::getSymbolID(Predefined::length),
          lengthDesc);
      (void)lengthPresent;
      assert(lengthPresent && ".length must be present in JSArray");

      if (!lengthDesc.flags.writable) {
        if (opFlags.getThrowOnError()) {
          return runtime->raiseTypeError(
              "Cannot assign to read-only 'length' property of array");
        }
        return false;
      }

      updateLength = true;
    }
  }

  bool newIsIndexed = canNewPropertyBeIndexed(dpFlags);
  if (newIsIndexed) {
    auto result = setOwnIndexed(
        selfHandle,
        runtime,
        *arrayIndex,
        dpFlags.setValue ? valueOrAccessor : runtime->getUndefinedValue());
    if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    if (!*result) {
      if (opFlags.getThrowOnError()) {
        // TODO: better error message.
        return runtime->raiseTypeError("Cannot define property");
      }
      return false;
    }
  }

  // If this is an array and we need to update ".length", do so.
  if (updateLength) {
    // This should always succeed since we are simply enlarging the length.
    auto res = JSArray::setLength(
        Handle<JSArray>::vmcast(selfHandle), runtime, *arrayIndex + 1, opFlags);
    (void)res;
    assert(
        res != ExecutionStatus::EXCEPTION && *res &&
        "JSArray::setLength() failed unexpectedly");
  }

  if (newIsIndexed)
    return true;

  // We are adding a new property with an index-like name.
  LAZY_TO_IDENTIFIER(runtime, nameValHandle, strPrim, id);
  return addOwnProperty(
      selfHandle, runtime, id, dpFlags, valueOrAccessor, opFlags);
}

CallResult<bool> JSObject::defineOwnComputed(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    Handle<> nameValHandle,
    DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return defineOwnComputedPrimitive(
      selfHandle, runtime, *converted, dpFlags, valueOrAccessor, opFlags);
}

std::pair<uint32_t, uint32_t> JSObject::_getOwnIndexedRangeImpl(
    JSObject *self) {
  return {0, 0};
}

bool JSObject::_haveOwnIndexedImpl(JSObject *self, Runtime *, uint32_t) {
  return false;
}

OptValue<PropertyFlags> JSObject::_getOwnIndexedPropertyFlagsImpl(
    JSObject *self,
    Runtime *runtime,
    uint32_t) {
  return llvm::None;
}

HermesValue JSObject::_getOwnIndexedImpl(JSObject *, Runtime *, uint32_t) {
  return HermesValue::encodeEmptyValue();
}

CallResult<bool>
JSObject::_setOwnIndexedImpl(Handle<JSObject>, Runtime *, uint32_t, Handle<>) {
  return false;
}

bool JSObject::_deleteOwnIndexedImpl(Handle<JSObject>, Runtime *, uint32_t) {
  return false;
}

bool JSObject::_checkAllOwnIndexedImpl(
    JSObject * /*self*/,
    ObjectVTable::CheckAllOwnIndexedMode /*mode*/) {
  return true;
}

void JSObject::preventExtensions(JSObject *self) {
  self->flags_.noExtend = true;
}

void JSObject::seal(Handle<JSObject> selfHandle, Runtime *runtime) {
  // Already sealed?
  if (selfHandle->flags_.sealed)
    return;

  auto newClazz = HiddenClass::makeAllNonConfigurable(
      runtime->makeHandle(selfHandle->clazz_), runtime);
  selfHandle->clazz_.set(*newClazz, &runtime->getHeap());

  selfHandle->flags_.sealed = true;
  selfHandle->flags_.noExtend = true;
}

void JSObject::freeze(Handle<JSObject> selfHandle, Runtime *runtime) {
  // Already frozen?
  if (selfHandle->flags_.frozen)
    return;

  auto newClazz = HiddenClass::makeAllReadOnly(
      runtime->makeHandle(selfHandle->clazz_), runtime);
  selfHandle->clazz_.set(*newClazz, &runtime->getHeap());

  selfHandle->flags_.frozen = true;
  selfHandle->flags_.sealed = true;
  selfHandle->flags_.noExtend = true;
}

void JSObject::updatePropertyFlagsWithoutTransitions(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    PropertyFlags flagsToClear,
    PropertyFlags flagsToSet,
    OptValue<llvm::ArrayRef<SymbolID>> props) {
  auto newClazz = HiddenClass::updatePropertyFlagsWithoutTransitions(
      runtime->makeHandle(selfHandle->clazz_),
      runtime,
      flagsToClear,
      flagsToSet,
      props);
  selfHandle->clazz_.set(*newClazz, &runtime->getHeap());
}

bool JSObject::isSealed(PseudoHandle<JSObject> self, Runtime *runtime) {
  if (self->flags_.sealed)
    return true;
  if (!self->flags_.noExtend)
    return false;

  auto selfHandle = toHandle(runtime, std::move(self));

  if (!HiddenClass::areAllNonConfigurable(
          runtime->makeHandle(selfHandle->clazz_), runtime)) {
    return false;
  }

  if (!checkAllOwnIndexed(
          *selfHandle, ObjectVTable::CheckAllOwnIndexedMode::NonConfigurable)) {
    return false;
  }

  // Now that we know we are sealed, set the flag.
  selfHandle->flags_.sealed = true;
  return true;
}

bool JSObject::isFrozen(PseudoHandle<JSObject> self, Runtime *runtime) {
  if (self->flags_.frozen)
    return true;
  if (!self->flags_.noExtend)
    return false;

  auto selfHandle = toHandle(runtime, std::move(self));

  if (!HiddenClass::areAllReadOnly(
          runtime->makeHandle(selfHandle->clazz_), runtime)) {
    return false;
  }

  if (!checkAllOwnIndexed(
          *selfHandle, ObjectVTable::CheckAllOwnIndexedMode::ReadOnly)) {
    return false;
  }

  // Now that we know we are sealed, set the flag.
  selfHandle->flags_.frozen = true;
  selfHandle->flags_.sealed = true;
  return true;
}

CallResult<bool> JSObject::addOwnProperty(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  /// Can we add more properties?
  if (!selfHandle->isExtensible() && !opFlags.getInternalForce()) {
    if (opFlags.getThrowOnError()) {
      return runtime->raiseTypeError(
          TwineChar16("Cannot add new property '") +
          runtime->getIdentifierTable().getStringView(runtime, name) + "'");
    }
    return false;
  }

  PropertyFlags flags{};

  // Accessors don't set writeable.
  if (dpFlags.isAccessor()) {
    dpFlags.setWritable = 0;
    flags.accessor = 1;
  }

  // Override the default flags if specified.
  if (dpFlags.setEnumerable)
    flags.enumerable = dpFlags.enumerable;
  if (dpFlags.setWritable)
    flags.writable = dpFlags.writable;
  if (dpFlags.setConfigurable)
    flags.configurable = dpFlags.configurable;
  flags.internalSetter = dpFlags.enableInternalSetter;

  if (LLVM_UNLIKELY(
          addOwnPropertyImpl(
              selfHandle, runtime, name, flags, valueOrAccessor) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return true;
}

ExecutionStatus JSObject::addOwnPropertyImpl(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    PropertyFlags propertyFlags,
    Handle<> valueOrAccessor) {
  // Add a new property to the class.
  // TODO: if we check for OOM here in the future, we must undo the slot
  // allocation.
  auto addResult = HiddenClass::addProperty(
      runtime->makeHandle(selfHandle->clazz_), runtime, name, propertyFlags);
  if (LLVM_UNLIKELY(addResult == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  selfHandle->clazz_.set(*addResult->first, &runtime->getHeap());

  allocateNewSlotStorage(
      selfHandle, runtime, addResult->second, valueOrAccessor);

  // If this is an index-like property, we need to clear the fast path flags.
  if (LLVM_UNLIKELY(selfHandle->clazz_->getHasIndexLikeProperties()))
    selfHandle->flags_.fastIndexProperties = false;

  return ExecutionStatus::RETURNED;
}

CallResult<bool> JSObject::updateOwnProperty(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    HiddenClass::PropertyPos propertyPos,
    NamedPropertyDescriptor desc,
    const DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  auto updateStatus = checkPropertyUpdate(
      runtime,
      desc.flags,
      dpFlags,
      getNamedSlotValue(selfHandle.get(), desc),
      valueOrAccessor,
      opFlags);
  if (updateStatus == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  if (updateStatus->first == PropertyUpdateStatus::failed)
    return false;

  // If the property flags changed, update them.
  if (updateStatus->second != desc.flags) {
    desc.flags = updateStatus->second;
    auto newClazz = HiddenClass::updateProperty(
        runtime->makeHandle(selfHandle->clazz_),
        runtime,
        propertyPos,
        desc.flags);
    selfHandle->clazz_.set(*newClazz, &runtime->getHeap());
  }

  if (updateStatus->first == PropertyUpdateStatus::done)
    return true;
  assert(
      updateStatus->first == PropertyUpdateStatus::needSet &&
      "unexpected PropertyUpdateStatus");

  if (dpFlags.setValue) {
    if (LLVM_LIKELY(!desc.flags.internalSetter))
      setNamedSlotValue(selfHandle.get(), runtime, desc, valueOrAccessor.get());
    else
      return internalSetter(
          selfHandle, runtime, name, desc, valueOrAccessor, opFlags);
  } else if (dpFlags.isAccessor())
    setNamedSlotValue(selfHandle.get(), runtime, desc, valueOrAccessor.get());

  return true;
}

CallResult<std::pair<JSObject::PropertyUpdateStatus, PropertyFlags>>
JSObject::checkPropertyUpdate(
    Runtime *runtime,
    const PropertyFlags currentFlags,
    DefinePropertyFlags dpFlags,
    const HermesValue curValueOrAccessor,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  // 8.12.9 [5] Return true, if every field in Desc is absent.
  if (dpFlags.isEmpty())
    return std::make_pair(PropertyUpdateStatus::done, currentFlags);

  assert(
      (!dpFlags.isAccessor() || (!dpFlags.setWritable && !dpFlags.writable)) &&
      "can't set both accessor and writable");
  assert(
      !dpFlags.enableInternalSetter &&
      "cannot change the value of internalSetter");

  // 8.12.9 [6] Return true, if every field in Desc also occurs in current and
  // the value of every field in Desc is the same value as the corresponding
  // field in current when compared using the SameValue algorithm (9.12).
  // TODO: this would probably be much more efficient with bitmasks.
  if ((!dpFlags.setEnumerable ||
       dpFlags.enumerable == currentFlags.enumerable) &&
      (!dpFlags.setWritable || dpFlags.writable == currentFlags.writable) &&
      (!dpFlags.setConfigurable ||
       dpFlags.configurable == currentFlags.configurable)) {
    if (dpFlags.isAccessor()) {
      if (currentFlags.accessor) {
        auto *curAccessor = vmcast<PropertyAccessor>(curValueOrAccessor);
        auto *newAccessor = vmcast<PropertyAccessor>(valueOrAccessor.get());

        if ((!dpFlags.setGetter ||
             curAccessor->getter == newAccessor->getter) &&
            (!dpFlags.setSetter ||
             curAccessor->setter == newAccessor->setter)) {
          return std::make_pair(PropertyUpdateStatus::done, currentFlags);
        }
      }
    } else if (dpFlags.setValue) {
      if (isSameValue(curValueOrAccessor, valueOrAccessor.get()))
        return std::make_pair(PropertyUpdateStatus::done, currentFlags);
    } else {
      return std::make_pair(PropertyUpdateStatus::done, currentFlags);
    }
  }

  // 8.12.9 [7]
  // If the property is not configurable, some aspects are not changeable.
  if (!currentFlags.configurable) {
    // Trying to change non-configurable to configurable?
    if (dpFlags.configurable) {
      if (opFlags.getThrowOnError()) {
        return runtime->raiseTypeError(
            "property is not configurable"); // TODO: better message.
      }
      return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
    }

    // Trying to change the enumerability of non-configurable property?
    if (dpFlags.setEnumerable &&
        dpFlags.enumerable != currentFlags.enumerable) {
      if (opFlags.getThrowOnError()) {
        return runtime->raiseTypeError(
            "property is not configurable"); // TODO: better message.
      }
      return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
    }
  }

  PropertyFlags newFlags = currentFlags;

  // 8.12.9 [8] If IsGenericDescriptor(Desc) is true, then no further validation
  // is required.
  if (!(dpFlags.setValue || dpFlags.setWritable || dpFlags.setGetter ||
        dpFlags.setSetter)) {
    // Do nothing
  }
  // 8.12.9 [9]
  // Changing between accessor and data descriptor?
  else if (currentFlags.accessor != dpFlags.isAccessor()) {
    if (!currentFlags.configurable) {
      if (opFlags.getThrowOnError()) {
        return runtime->raiseTypeError(
            "property is not configurable"); // TODO: better message.
      }
      return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
    }

    // If we change from accessor to data descriptor, Preserve the existing
    // values of the converted property’s [[Configurable]] and [[Enumerable]]
    // attributes and set the rest of the property’s attributes to their default
    // values.
    // If it's the other way around, since the accessor doesn't have the
    // [[Writable]] attribute, do nothing.
    newFlags.writable = 0;
  }
  // 8.12.9 [10] if both are data descriptors.
  else if (!currentFlags.accessor) {
    if (!currentFlags.configurable) {
      if (!currentFlags.writable) {
        // If the current property is not writable, but the new one is.
        if (dpFlags.writable) {
          if (opFlags.getThrowOnError()) {
            return runtime->raiseTypeError(
                "property is not configurable"); // TODO: better message.
          }
          return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
        }

        // If we are setting a different value.
        if (dpFlags.setValue &&
            !isSameValue(curValueOrAccessor, valueOrAccessor.get())) {
          if (opFlags.getThrowOnError()) {
            return runtime->raiseTypeError(
                "property is not writable"); // TODO: better message.
          }
          return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
        }
      }
    }
  }
  // 8.12.9 [11] Both are accessors.
  else {
    auto *curAccessor = vmcast<PropertyAccessor>(curValueOrAccessor);
    auto *newAccessor = vmcast<PropertyAccessor>(valueOrAccessor.get());

    // If not configurable, make sure that nothing is changing.
    if (!currentFlags.configurable) {
      if ((dpFlags.setGetter && newAccessor->getter != curAccessor->getter) ||
          (dpFlags.setSetter && newAccessor->setter != curAccessor->setter)) {
        if (opFlags.getThrowOnError()) {
          return runtime->raiseTypeError(
              "property is not configurable"); // TODO: better message.
        }
        return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
      }
    }

    // If not setting the getter or the setter, re-use the current one.
    if (!dpFlags.setGetter)
      newAccessor->getter.set(curAccessor->getter, &runtime->getHeap());
    if (!dpFlags.setSetter)
      newAccessor->setter.set(curAccessor->setter, &runtime->getHeap());
  }

  // 8.12.9 [12] For each attribute field of Desc that is present, set the
  // correspondingly named attribute of the property named P of object O to the
  // value of the field.
  if (dpFlags.setEnumerable)
    newFlags.enumerable = dpFlags.enumerable;
  if (dpFlags.setWritable)
    newFlags.writable = dpFlags.writable;
  if (dpFlags.setConfigurable)
    newFlags.configurable = dpFlags.configurable;

  if (dpFlags.setValue)
    newFlags.accessor = false;
  else if (dpFlags.isAccessor())
    newFlags.accessor = true;
  else
    return std::make_pair(PropertyUpdateStatus::done, newFlags);

  return std::make_pair(PropertyUpdateStatus::needSet, newFlags);
}

CallResult<bool> JSObject::internalSetter(
    Handle<JSObject> selfHandle,
    Runtime *runtime,
    SymbolID name,
    NamedPropertyDescriptor /*desc*/,
    Handle<> value,
    PropOpFlags opFlags) {
  if (vmisa<JSArray>(selfHandle.get())) {
    if (name == Predefined::getSymbolID(Predefined::length)) {
      return JSArray::setLength(
          Handle<JSArray>::vmcast(selfHandle), runtime, value, opFlags);
    }
  }

  llvm_unreachable("unhandled property in Object::internalSetter()");
}

namespace {

/// Helper function to add all the property names of an object to an
/// array, starting at the given index. Only enumerable properties are
/// incluced. Returns the index after the last property added, but...
CallResult<uint32_t> appendAllPropertyNames(
    Handle<JSObject> obj,
    Runtime *runtime,
    MutableHandle<BigStorage> &arr,
    uint32_t beginIndex) {
  uint32_t size = beginIndex;
  // We know that duplicate property names can only exist between objects in
  // the prototype chain. Hence there should not be duplicated properties
  // before we start to look at any prototype.
  bool needDedup = false;
  MutableHandle<> prop(runtime);
  MutableHandle<JSObject> head(runtime, obj.get());
  MutableHandle<StringPrimitive> tmpVal{runtime};
  while (head.get()) {
    GCScope gcScope(runtime);

    // enumerableProps will contain all enumerable own properties from obj.
    auto cr =
        JSObject::getOwnPropertyNames(head, runtime, true /* onlyEnumerable */);
    if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto enumerableProps = *cr;
    auto marker = gcScope.createMarker();

    for (unsigned i = 0, e = enumerableProps->getEndIndex(); i < e; ++i) {
      gcScope.flushToMarker(marker);
      prop = enumerableProps->at(i);
      if (!needDedup) {
        // If no dedup is needed, add it directly.
        if (LLVM_UNLIKELY(
                BigStorage::push_back(arr, runtime, prop) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        ++size;
        continue;
      }
      // Otherwise loop through all existing properties and check if we
      // have seen it before.
      bool dupFound = false;
      if (prop->isNumber()) {
        for (uint32_t j = beginIndex; j < size && !dupFound; ++j) {
          HermesValue val = arr->at(j);
          if (val.isNumber()) {
            dupFound = val.getNumber() == prop->getNumber();
          } else {
            // val is string, prop is number.
            tmpVal = val.getString();
            auto valNum = toArrayIndex(
                StringPrimitive::createStringView(runtime, tmpVal));
            dupFound = valNum && valNum.getValue() == prop->getNumber();
          }
        }
      } else {
        for (uint32_t j = beginIndex; j < size && !dupFound; ++j) {
          HermesValue val = arr->at(j);
          if (val.isNumber()) {
            // val is number, prop is string.
            auto propNum = toArrayIndex(StringPrimitive::createStringView(
                runtime, Handle<StringPrimitive>::vmcast(prop)));
            dupFound = propNum && (propNum.getValue() == val.getNumber());
          } else {
            dupFound = val.getString()->equals(prop->getString());
          }
        }
      }
      if (LLVM_LIKELY(!dupFound)) {
        if (LLVM_UNLIKELY(
                BigStorage::push_back(arr, runtime, prop) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        ++size;
      }
    }
    // Continue to follow the prototype chain.
    head = head->getParent();
    needDedup = true;
  }
  return size;
}

/// Adds the hidden classes of the prototype chain of obj to arr,
/// starting with the prototype of obj at index 0, etc., and
/// terminates with null.
///
/// \param obj The object whose prototype chain should be output
/// \param[out] arr The array where the classes will be appended. This
/// array is cleared if any object is unsuitable for caching.
ExecutionStatus setProtoClasses(
    Runtime *runtime,
    Handle<JSObject> obj,
    MutableHandle<BigStorage> &arr) {
  // Layout of a JSArray stored in the for-in cache:
  // [class(proto(obj)), class(proto(proto(obj))), ..., null, prop0, prop1, ...]

  if (!obj->shouldCacheForIn()) {
    arr->clear();
    return ExecutionStatus::RETURNED;
  }
  MutableHandle<JSObject> head(runtime, obj->getParent());
  MutableHandle<> clazz(runtime);
  GCScopeMarkerRAII marker{runtime};
  while (head.get()) {
    if (!head->shouldCacheForIn()) {
      arr->clear();
      return ExecutionStatus::RETURNED;
    }
    clazz = HermesValue::encodeObjectValue(head->getClass());
    if (LLVM_UNLIKELY(
            BigStorage::push_back(arr, runtime, clazz) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    head = head->getParent();
    marker.flush();
  }
  clazz = HermesValue::encodeNullValue();
  return BigStorage::push_back(arr, runtime, clazz);
}

/// Verifies that the classes of obj's prototype chain still matches those
/// previously prefixed to arr by setProtoClasses.
///
/// \param obj The object whose prototype chain should be verified
/// \param arr Array previously populated by setProtoClasses
/// \return The index after the terminating null if everything matches,
/// otherwise 0.
uint32_t matchesProtoClasses(
    Runtime *runtime,
    Handle<JSObject> obj,
    Handle<BigStorage> arr) {
  MutableHandle<JSObject> head(runtime, obj->getParent());
  uint32_t i = 0;
  while (head.get()) {
    HermesValue protoCls = arr->at(i++);
    if (protoCls.isNull() || protoCls.getObject() != head->getClass()) {
      return 0;
    }
    head = head->getParent();
  }
  // The chains must both end at the same point.
  if (head || !arr->at(i++).isNull()) {
    return 0;
  }
  assert(i > 0 && "success should be positive");
  return i;
}

} // namespace

CallResult<Handle<BigStorage>> getForInPropertyNames(
    Runtime *runtime,
    Handle<JSObject> obj,
    uint32_t &beginIndex,
    uint32_t &endIndex) {
  Handle<HiddenClass> clazz(runtime, obj->getClass());

  // Fast case: Check the cache.
  MutableHandle<BigStorage> arr(runtime, clazz->getForInCache());
  if (arr) {
    beginIndex = matchesProtoClasses(runtime, obj, arr);
    if (beginIndex) {
      // Cache is valid for this object, so use it.
      endIndex = arr->size();
      return arr;
    }
    // Invalid for this object. We choose to clear the cache since the
    // changes to the prototype chain probably affect other objects too.
    clazz->clearForInCache();
    // Clear arr to slightly reduce risk of OOM from allocation below.
    arr = nullptr;
  }

  // Slow case: Build the array of properties.
  auto ownPropEstimate = clazz->getNumProperties();
  auto arrRes = obj->shouldCacheForIn()
      ? BigStorage::createLongLived(runtime, ownPropEstimate)
      : BigStorage::create(runtime, ownPropEstimate);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  arr = vmcast<BigStorage>(*arrRes);
  if (setProtoClasses(runtime, obj, arr) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  beginIndex = arr->size();
  // If obj or any of its prototypes are unsuitable for caching, then
  // beginIndex is 0 and we return an array with only the property names.
  bool canCache = beginIndex;
  auto end = appendAllPropertyNames(obj, runtime, arr, beginIndex);
  if (end == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  endIndex = *end;
  // Avoid degenerate memory explosion: if > 75% of the array is properties
  // or classes from prototypes, then don't cache it.
  const bool tooMuchProto = *end / 4 > ownPropEstimate;
  if (canCache && !tooMuchProto) {
    assert(beginIndex > 0 && "cached array must start with proto classes");
#ifdef HERMES_SLOW_DEBUG
    assert(beginIndex == matchesProtoClasses(runtime, obj, arr) && "matches");
#endif
    clazz->setForInCache(*arr, runtime);
  }
  return arr;
}

//===----------------------------------------------------------------------===//
// class PropertyAccessor

VTable PropertyAccessor::vt{CellKind::PropertyAccessorKind,
                            sizeof(PropertyAccessor)};

void PropertyAccessorBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const PropertyAccessor *>(cell);
  mb.addField("@getter", &self->getter);
  mb.addField("@setter", &self->setter);
}

CallResult<HermesValue> PropertyAccessor::create(
    Runtime *runtime,
    Handle<Callable> getter,
    Handle<Callable> setter) {
  void *mem = runtime->alloc(sizeof(PropertyAccessor));
  return HermesValue::encodeObjectValue(
      new (mem) PropertyAccessor(runtime, *getter, *setter));
}

} // namespace vm
} // namespace hermes
