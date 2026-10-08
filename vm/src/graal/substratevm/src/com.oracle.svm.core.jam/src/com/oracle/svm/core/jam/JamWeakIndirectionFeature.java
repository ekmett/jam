// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
package com.oracle.svm.core.jam;

import java.lang.reflect.Modifier;
import com.oracle.svm.core.ParsingReason;
import com.oracle.svm.core.SubstrateOptions;
import com.oracle.svm.core.feature.InternalFeature;
import com.oracle.svm.core.graal.nodes.FieldOffsetNode;
import com.oracle.svm.guest.staging.util.UserError;
import com.oracle.svm.hosted.snippets.SubstrateGraphBuilderPlugins;
import com.oracle.svm.shared.feature.AutomaticallyRegisteredFeature;
import com.oracle.svm.shared.util.ReflectionUtil;
import jdk.graal.compiler.nodes.ValueNode;
import jdk.graal.compiler.nodes.CallTargetNode.InvokeKind;
import jdk.graal.compiler.nodes.graphbuilderconf.GraphBuilderConfiguration.Plugins;
import jdk.graal.compiler.nodes.graphbuilderconf.GraphBuilderContext;
import jdk.graal.compiler.nodes.graphbuilderconf.InvocationPlugin.RequiredInvocationPlugin;
import jdk.graal.compiler.nodes.graphbuilderconf.InvocationPlugins.Registration;
import jdk.graal.compiler.phases.util.Providers;
import jdk.vm.ci.meta.JavaKind;
import jdk.vm.ci.meta.ResolvedJavaField;
import jdk.vm.ci.meta.ResolvedJavaMethod;
import jdk.vm.ci.meta.ResolvedJavaType;

/** Validate named fields during analysis and retain them through the standard unsafe-offset nodes. */
@AutomaticallyRegisteredFeature
final class JamWeakIndirectionFeature implements InternalFeature {
    @Override public boolean isInConfiguration(IsInConfigurationAccess access) { return SubstrateOptions.useJamGC() && access.findClassByName("jam.vm.Weak") != null; }

    @Override public void registerInvocationPlugins(Providers providers, Plugins plugins, ParsingReason reason) {
        Registration registration = new Registration(plugins.getInvocationPlugins(), "jam.vm.Weak");
        registration.register(new RequiredInvocationPlugin("registerIndirection", Class.class, String.class, int.class, String.class) {
            @Override public boolean apply(GraphBuilderContext b, ResolvedJavaMethod targetMethod, Receiver receiver,
                            ValueNode carrier, ValueNode stateName, ValueNode completedState, ValueNode referentName) {
                ResolvedJavaType type = SubstrateGraphBuilderPlugins.asConstantType(b, carrier);
                String state = SubstrateGraphBuilderPlugins.asConstantObject(b, String.class, stateName);
                String referent = SubstrateGraphBuilderPlugins.asConstantObject(b, String.class, referentName);
                UserError.guarantee(type != null && state != null && referent != null,
                                "jam.vm.Weak.registerIndirection requires a constant carrier class and constant field names in Native Image");
                ResolvedJavaField stateField = declaredField(type, state);
                ResolvedJavaField referentField = declaredField(type, referent);
                UserError.guarantee(stateField != null && stateField.getJavaKind() == JavaKind.Int && Modifier.isVolatile(stateField.getModifiers()),
                                "Weak indirection state must be a declared nonstatic volatile int: %s.%s", type.toJavaName(), state);
                UserError.guarantee(referentField != null && referentField.getType().equals(b.getMetaAccess().lookupJavaType(Object.class)),
                                "Weak indirection referent must be a declared nonstatic Object: %s.%s", type.toJavaName(), referent);
                ResolvedJavaMethod register = b.getMetaAccess().lookupJavaMethod(ReflectionUtil.lookupMethod(JamWeakIndirections.class,
                                "register", Class.class, long.class, int.class, long.class));
                // FieldOffsetNode registers unsafe access during analysis and resolves only after target layout.
                ValueNode stateOffset = b.add(FieldOffsetNode.create(JavaKind.Long, stateField));
                ValueNode referentOffset = b.add(FieldOffsetNode.create(JavaKind.Long, referentField));
                b.handleReplacedInvoke(InvokeKind.Static, register, new ValueNode[]{carrier, stateOffset, completedState, referentOffset}, false);
                return true;
            }
        });
    }

    private static ResolvedJavaField declaredField(ResolvedJavaType type, String name) {
        for (ResolvedJavaField field : type.getInstanceFields(false)) {
            if (field.getDeclaringClass().equals(type) && field.getName().equals(name) && !Modifier.isStatic(field.getModifiers())) return field;
        }
        return null;
    }
}
