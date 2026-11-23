export module vrento:vulkan.resource_bindings;
import rstd;
import :pass.resources;
import :vulkan.frame;
import :resource_registry.prepared;

export namespace vrento::vulkan
{
struct TextureSelection {
    resource::TextureUseHandle use;
    texture::TextureViewRef    view;
    usize                      image_slot {};
};

inline auto capture_resources(const pass::GraphResourceMap&                   mapping,
                              const resource_registry::PreparedResourceTable& prepared,
                              slice<TextureSelection> selections, GraphFrameRequest& frame)
    -> Result<empty, GraphExecutionError> {
    if (mapping.scope != frame.bindings.scope() || prepared.Generation() != u64(mapping.scope))
        return Err(GraphExecutionError { GraphExecutionStage::Binding });
    for (const auto& external : mapping.externals)
        if (! frame.bindings.resolve({ external.version_index }))
            return Err(GraphExecutionError {
                GraphExecutionStage::Binding, {}, external.version.resource });
    HashMap<resource::TextureUseHandle, usize> chosen;
    for (usize i {}; i < selections.len(); ++i) {
        const auto& selection = selections[i];
        if (auto old = chosen.get(selection.use)) {
            const auto& view = selections[**old].view;
            if (view.texture().identity() != selection.view.texture().identity() ||
                view.description() != selection.view.description())
                return Err(GraphExecutionError { GraphExecutionStage::Binding });
        } else {
            (void)chosen.insert(selection.use, i);
        }
    }
    for (const auto& binding : mapping.textures) {
        auto version = binding.resource.version;
        auto value   = prepared.Resolve(binding.resource.handle);
        if (! value)
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        if ((**value).definition.id() != binding.definition.id())
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        auto image_slot = (**value).image.active_slot();
        auto selected   = (**value).TextureAt(image_slot);
        if (selected.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Binding,
                {},
                version.resource,
                vrento::vulkan::GraphExecutionCause::Texture(selected.unwrap_err()) });
        auto view = selected->view;
        if (auto index = chosen.get(binding.resource.handle)) {
            view       = selections[**index].view;
            image_slot = selections[**index].image_slot;
        }
        if (! view.valid())
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        const auto& range    = view.description().range;
        const auto& required = binding.range;
        if (required.aspect != (range.aspect == texture::Aspect::Depth ? 2u : 1u) ||
            required.first_mip > range.mip_count ||
            required.mip_count > range.mip_count - required.first_mip ||
            required.first_layer > range.layer_count ||
            required.layer_count > range.layer_count - required.first_layer)
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        GraphBindingSlot slot { binding.version_index };
        if (auto existing = frame.bindings.resolve(slot)) {
            auto previous = (*existing)->Texture();
            if (! previous || previous->texture().identity() != view.texture().identity() ||
                previous->description() != view.description())
                return Err(
                    GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
            continue;
        }
        auto bound = frame.bindings.bind(slot,
                                         Arc<dyn<GraphBinding>>::make(GraphTextureBinding(view)),
                                         (**value).PhysicalBinding(image_slot));
        if (bound.is_err()) return Err(bound.unwrap_err());
    }
    for (const auto& binding : mapping.buffers) {
        auto version = binding.resource.version;
        auto value   = prepared.Resolve(binding.resource.use);
        if (! value)
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        if ((**value).buffer.definition.id() != binding.definition.id())
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        const auto& allocation = (**value).buffer.physical->buffer;
        if (binding.range.offset > allocation.size() ||
            binding.range.size > allocation.size() - binding.range.offset)
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        frame.buffers.push(
            { { binding.version_index }, allocation.clone(), (**value).physical_binding });
    }
    return Ok(empty {});
}
} // namespace vrento::vulkan
