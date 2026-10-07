// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
package com.oracle.svm.core.jam;

import com.oracle.svm.core.heap.BarrierSetProvider;
import com.oracle.svm.shared.singletons.traits.BuiltinTraits.BuildtimeAccessOnly;
import com.oracle.svm.shared.singletons.traits.BuiltinTraits.NoLayeredCallbacks;
import com.oracle.svm.shared.singletons.traits.SingletonTraits;
import jdk.graal.compiler.core.common.memory.BarrierType;
import jdk.graal.compiler.nodes.gc.BarrierSet;
import jdk.graal.compiler.nodes.gc.CardTableBarrierSet;
import jdk.vm.ci.meta.JavaKind;
import jdk.vm.ci.meta.MetaAccessProvider;
import jdk.vm.ci.meta.ResolvedJavaField;

/** Reuse Graal's card-table barrier insertion for stores, atomics and array copies. */
@SingletonTraits(access = BuildtimeAccessOnly.class, layeredCallbacks = NoLayeredCallbacks.class)
final class JamBarrierSetProvider implements BarrierSetProvider {
    @Override
    public BarrierSet createBarrierSet(MetaAccessProvider metaAccess) {
        return new CardTableBarrierSet(metaAccess.lookupJavaType(Object[].class), true) {
            @Override
            public BarrierType fieldWriteBarrierType(ResolvedJavaField field, JavaKind kind) {
                return field.isStatic() && kind == JavaKind.Object ? arrayWriteBarrierType(kind) : super.fieldWriteBarrierType(field, kind);
            }
        };
    }
}
