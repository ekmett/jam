// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
package com.oracle.svm.core.jam;

import java.util.Map;
import org.graalvm.word.LocationIdentity;
import org.graalvm.word.Pointer;
import org.graalvm.word.impl.Word;
import com.oracle.svm.core.graal.snippets.NodeLoweringProvider;
import com.oracle.svm.core.graal.snippets.SubstrateTemplates;
import jdk.graal.compiler.api.replacements.Snippet;
import jdk.graal.compiler.graph.Node;
import jdk.graal.compiler.nodes.NamedLocationIdentity;
import jdk.graal.compiler.nodes.extended.FixedValueAnchorNode;
import jdk.graal.compiler.nodes.gc.SerialArrayRangeWriteBarrierNode;
import jdk.graal.compiler.nodes.gc.SerialWriteBarrierNode;
import jdk.graal.compiler.nodes.memory.address.OffsetAddressNode;
import jdk.graal.compiler.options.OptionValues;
import jdk.graal.compiler.phases.util.Providers;
import jdk.graal.compiler.replacements.SnippetTemplate;
import jdk.graal.compiler.replacements.SnippetTemplate.Arguments;
import jdk.graal.compiler.replacements.SnippetTemplate.SnippetInfo;
import jdk.graal.compiler.replacements.Snippets;

/** Dirty the owner's head card; collection visits the complete object exactly once. */
final class JamBarrierSnippets extends SubstrateTemplates implements Snippets {
    private static final LocationIdentity CARD_LOCATION = NamedLocationIdentity.mutable("Jam cards");
    private final SnippetInfo postWrite;

    @SuppressWarnings("this-escape")
    JamBarrierSnippets(OptionValues options, Providers providers) {
        super(options, providers);
        postWrite = snippet(providers, JamBarrierSnippets.class, "postWrite", CARD_LOCATION);
    }

    @Snippet
    public static void postWrite(Object owner) {
        Object anchored = FixedValueAnchorNode.getObject(owner);
        Pointer address = Word.objectToUntrackedPointer(anchored);
        JamHeap heap = JamHeap.get();
        if (address.aboveOrEqual(heap.oldBegin()) && address.belowThan(heap.oldEnd())) {
            heap.cardTable().writeByte(address.subtract(heap.base()).unsignedShiftRight(JamHeap.CARD_SHIFT), (byte) 1, CARD_LOCATION);
        }
    }

    void registerLowerings(Map<Class<? extends Node>, NodeLoweringProvider<?>> lowerings) {
        lowerings.put(SerialWriteBarrierNode.class, (NodeLoweringProvider<SerialWriteBarrierNode>) (node, tool) -> {
            Arguments args = new Arguments(postWrite, node.graph(), tool.getLoweringStage());
            args.add("owner", ((OffsetAddressNode) node.getAddress()).getBase());
            template(tool, node, args).instantiate(tool.getMetaAccess(), node, SnippetTemplate.DEFAULT_REPLACER, args);
        });
        lowerings.put(SerialArrayRangeWriteBarrierNode.class, (NodeLoweringProvider<SerialArrayRangeWriteBarrierNode>) (node, tool) -> {
            Arguments args = new Arguments(postWrite, node.graph(), tool.getLoweringStage());
            args.add("owner", ((OffsetAddressNode) node.getAddress()).getBase());
            template(tool, node, args).instantiate(tool.getMetaAccess(), node, SnippetTemplate.DEFAULT_REPLACER, args);
        });
    }
}
