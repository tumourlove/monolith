#include "MonolithBlueprintEditCradle.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "Templates/Function.h"

namespace MonolithEditCradle
{

static bool MayContainObjectRef(FProperty* Prop)
{
    if (!Prop) return false;
    if (CastField<FObjectProperty>(Prop))     return true;
    if (CastField<FSoftObjectProperty>(Prop)) return true;
    if (CastField<FWeakObjectProperty>(Prop)) return true;
    if (CastField<FInterfaceProperty>(Prop))  return true;
    if (CastField<FStructProperty>(Prop))     return true;  // may contain nested refs
    if (CastField<FArrayProperty>(Prop))      return true;  // inner may be object ref
    if (CastField<FMapProperty>(Prop))        return true;
    if (CastField<FSetProperty>(Prop))        return true;
    return false;
}

/** Fire Pre+Post for a single property chain */
static void FireEditNotification(UObject* Obj, const TArray<FProperty*>& PropertyChain)
{
    if (PropertyChain.Num() == 0 || !Obj) return;

    // Build FEditPropertyChain: root at head, leaf at tail.
    // AddTail in forward iteration produces [Root -> Struct -> Leaf] which is
    // the ordering UE expects (outermost at head, innermost at tail).
    FEditPropertyChain Chain;
    for (FProperty* P : PropertyChain)
    {
        Chain.AddTail(P);
    }
    Chain.SetActivePropertyNode(PropertyChain.Last());

    Obj->PreEditChange(Chain);

    FPropertyChangedEvent ChangedEvent(PropertyChain.Last(), EPropertyChangeType::ValueSet);
    FPropertyChangedChainEvent ChainEvent(Chain, ChangedEvent);
    Obj->PostEditChangeChainProperty(ChainEvent);
}

/** Returns true if this property IS an object-ref leaf */
static bool IsObjectRefLeaf(FProperty* Prop)
{
    return CastField<FObjectProperty>(Prop)
        || CastField<FSoftObjectProperty>(Prop)
        || CastField<FWeakObjectProperty>(Prop)
        || CastField<FInterfaceProperty>(Prop);
}

using FObjectRefLeafVisitor = TFunctionRef<void(
    FProperty* LeafProp,
    const void* LeafValuePtr,
    const TArray<FProperty*>& Chain)>;

/**
 * Walks Prop's property tree, invoking Visitor at every object-ref leaf.
 * ContainerPtr is the container-of-Prop: the UObject at the top, the enclosing
 * struct / array element / map slot as we descend. Chain accumulates the
 * root→leaf property path for cradle callers; reparent callers ignore it.
 */
static void WalkObjectRefLeaves(
    FProperty* Prop,
    const void* ContainerPtr,
    TArray<FProperty*>& Chain,
    const FObjectRefLeafVisitor& Visitor)
{
    const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(ContainerPtr);

    auto VisitInner = [&](FProperty* Inner, const void* InnerContainerPtr)
    {
        Chain.Add(Inner);

        if (IsObjectRefLeaf(Inner))
        {
            const void* LeafValuePtr = Inner->ContainerPtrToValuePtr<void>(InnerContainerPtr);
            Visitor(Inner, LeafValuePtr, Chain);
        }
        else if (MayContainObjectRef(Inner))
        {
            WalkObjectRefLeaves(Inner, InnerContainerPtr, Chain, Visitor);
        }

        Chain.Pop();
    };

    if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
    {
        for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
        {
            VisitInner(*It, ValuePtr);
        }
    }
    else if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
    {
        FScriptArrayHelper Helper(ArrayProp, ValuePtr);
        for (int32 i = 0; i < Helper.Num(); ++i)
        {
            // ArrayProp->Inner has offset 0 within each element, so ElemPtr
            // serves as both the container for Inner and Inner's value pointer.
            VisitInner(ArrayProp->Inner, Helper.GetRawPtr(i));
        }
    }
    else if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
    {
        // Sparse storage: iterate to GetMaxIndex, not Num (UnrealType.h:4579).
        FScriptMapHelper Helper(MapProp, ValuePtr);
        for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
        {
            if (!Helper.IsValidIndex(i)) continue;
            // KeyProp has offset 0, ValueProp has offset MapLayout.ValueOffset within
            // each hash-pair slot (FMapProperty::LinkInternal in PropertyMap.cpp:226).
            // Pass PairPtr as the shared container so ContainerPtrToValuePtr resolves
            // Key to PairPtr+0 and Value to PairPtr+ValueOffset. Passing GetValuePtr(i)
            // directly would double-offset (GetValuePtr already bakes in ValueOffset).
            const void* PairPtr = Helper.GetPairPtr(i);
            VisitInner(MapProp->KeyProp,   PairPtr);
            VisitInner(MapProp->ValueProp, PairPtr);
        }
    }
    else if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
    {
        // Sparse storage: see map case.
        FScriptSetHelper Helper(SetProp, ValuePtr);
        for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
        {
            if (!Helper.IsValidIndex(i)) continue;
            VisitInner(SetProp->ElementProp, Helper.GetElementPtr(i));
        }
    }
}

void FireFullCradle(UObject* Obj, FProperty* Prop)
{
    if (!Obj || !Prop) return;

    // If the property itself is an object-ref leaf, fire once for it
    if (IsObjectRefLeaf(Prop))
    {
        TArray<FProperty*> Chain = { Prop };
        FireEditNotification(Obj, Chain);
        return;
    }

    // Otherwise recurse into struct/array/map/set looking for nested object refs
    if (MayContainObjectRef(Prop))
    {
        TArray<FProperty*> Chain = { Prop };
        WalkObjectRefLeaves(Prop, Obj, Chain,
            [Obj](FProperty*, const void*, const TArray<FProperty*>& CurrentChain)
            {
                FireEditNotification(Obj, CurrentChain);
            });
    }
}

// --- Reparent transient-outer inline subobjects ---

static bool IsInstancedObjectLeaf(FProperty* Prop)
{
    if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
    {
        return ObjProp->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance);
    }
    return false;
}

static void ReparentIfTransient(UObject* TargetObject, UObject* Sub)
{
    if (!Sub) return;
    if (Sub->GetOutermost() != GetTransientPackage()) return;
    Sub->Rename(nullptr, TargetObject, REN_DontCreateRedirectors | REN_NonTransactional);
}

void ReparentTransientInstancedSubobjects(UObject* TargetObject, FProperty* Prop)
{
    if (!TargetObject || !Prop) return;

    if (IsInstancedObjectLeaf(Prop))
    {
        const FObjectProperty* ObjProp = CastFieldChecked<FObjectProperty>(Prop);
        UObject* Sub = ObjProp->GetObjectPropertyValue(
            Prop->ContainerPtrToValuePtr<void>(TargetObject));
        ReparentIfTransient(TargetObject, Sub);
        return;
    }

    if (MayContainObjectRef(Prop))
    {
        TArray<FProperty*> Chain = { Prop };
        WalkObjectRefLeaves(Prop, TargetObject, Chain,
            [TargetObject](FProperty* LeafProp, const void* LeafValuePtr, const TArray<FProperty*>&)
            {
                if (!IsInstancedObjectLeaf(LeafProp)) return;
                const FObjectProperty* ObjProp = CastFieldChecked<FObjectProperty>(LeafProp);
                UObject* Sub = ObjProp->GetObjectPropertyValue(LeafValuePtr);
                ReparentIfTransient(TargetObject, Sub);
            });
    }
}

} // namespace MonolithEditCradle
