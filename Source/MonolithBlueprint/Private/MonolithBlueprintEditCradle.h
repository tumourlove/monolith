#pragma once

#include "CoreMinimal.h"

namespace MonolithEditCradle
{
    /**
     * Walks Prop's property tree and fires PreEditChange/PostEditChangeChainProperty
     * for every leaf FObjectProperty/FSoftObjectProperty/FWeakObjectProperty/FInterfaceProperty.
     * This ensures FOverridableManager marks each inner property as overridden.
     * Does NOT wrap in transaction/Modify — caller is expected to do that.
     */
    void FireFullCradle(UObject* Obj, FProperty* Prop);

    /**
     * Reparents any Instanced / PersistentInstance subobject currently outered to
     * /Engine/Transient under TargetObject. Fixes the FJsonObjectConverter path
     * (Engine/Source/Runtime/JsonUtilities/Private/JsonObjectConverter.cpp:964) which
     * defaults Outer to GetTransientPackage() when the immediate container is a USTRUCT
     * rather than a UObject — without this, FLinkerSave nulls the ref at save time.
     *
     * Must run AFTER FJsonObjectConverter::JsonValueToUProperty and BEFORE FireFullCradle,
     * so the cradle's Pre/Post fires on the correctly-outered subobjects.
     *
     * The rename uses REN_NonTransactional — subobject outer changes are not in the undo
     * buffer. If the enclosing transaction is later undone, the JSON-written property
     * value reverts but the freshly-created subobjects remain outered to TargetObject
     * (harmless — they become orphans reclaimed by GC).
     */
    void ReparentTransientInstancedSubobjects(UObject* TargetObject, FProperty* Prop);
}
