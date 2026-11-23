export module vrento:vulkan.texture_output;
export import :vulkan.passes.graphics;
export import :vulkan.passes.surface;
export import :vulkan.frame;
import :vulkan.shader_compile;
import rstd;

namespace vrento::vulkan
{
constexpr auto fullscreen_vertex = R"(#version 320 es
layout(location = 0) out vec2 v_Texcoord;

const vec2 k_Position[4] = vec2[4](
    vec2(-1.0, -1.0),
    vec2(-1.0,  1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0));
const vec2 k_Texcoord[4] = vec2[4](
    vec2(0.0, 1.0),
    vec2(0.0, 0.0),
    vec2(1.0, 1.0),
    vec2(1.0, 0.0));

void main()
{
    v_Texcoord = k_Texcoord[gl_VertexIndex];
    gl_Position = vec4(k_Position[gl_VertexIndex], 0.0, 1.0);
}
)"_str;

}
export namespace vrento::vulkan
{
inline auto texture_output_layout(resource::PipelineUseHandle pipeline)
    -> PipelineLayoutRequirement {
    PipelineLayoutRequirement    result { .pipeline = pipeline };
    PipelineLayoutSetRequirement set { .set = u32(1) };
    set.bindings.push({ .binding          = u32(1),
                        .descriptor_type  = u32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                        .descriptor_count = u32(1),
                        .stage_flags      = u32(VK_SHADER_STAGE_FRAGMENT_BIT) });
    result.descriptor_sets.push(rstd::move(set));
    return result;
}
inline auto
texture_output_recipe(ref<dyn<ShaderBackend>> backend, resource::PipelineUseHandle pipeline,
                      resource::RenderPassUseHandle  render_pass,
                      resource::PipelineLayoutHandle layout, VkFormat format, bool opaque)
    -> Result<GraphicsDrawDesc, resource::ResourceError> {
    auto fragment = String::make(R"(#version 320 es
layout(location = 0) in vec2 v_Texcoord;
layout(location = 0) out vec4 out_FragColor;
layout(set = 1, binding = 1) uniform sampler2D u_Texture;
void main() {
    out_FragColor = texture(u_Texture, v_Texcoord);
)"_str);
    if (opaque) fragment.push_str("    out_FragColor.a = 1.0;\n"_str);
    fragment.push_str("}\n"_str);
    array<ShaderCompUnit, 2> units {
        ShaderCompUnit { ShaderType::VERTEX, String::make(fullscreen_vertex) },
        ShaderCompUnit { ShaderType::FRAGMENT, rstd::move(fragment) }
    };
    Vec<Uni_ShaderSpv> stages;
    auto               compiled = backend->CompileAndLinkShaderUnits(
        units.as_slice(),
        ShaderCompOpt { .target = VulkanTarget::Vulkan_1_1, .optimize = true },
        stages);
    if (compiled.is_err()) return Err(rstd::move(compiled).unwrap_err());
    GraphicsPipeline defaults;
    defaults.toDefault();
    PipelineResourceRequest request {
        .pipeline_layout = layout,
        .shader_stages   = rstd::move(stages),
        .color_blend  = { .blendEnable         = VK_FALSE,
                          .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                          .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                          .colorBlendOp        = VK_BLEND_OP_ADD,
                          .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                          .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                          .alphaBlendOp        = VK_BLEND_OP_ADD,
                          .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT },
        .depth        = defaults.depth,
        .raster       = defaults.raster,
        .multisample  = defaults.multisample,
        .topology     = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        .color_format = format,
        .color_initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .color_final_layout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .color_load_op        = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .has_color_attachment = true,
        .has_depth_attachment = false
    };
    return Ok(GraphicsDrawDesc { {}, pipeline, render_pass, rstd::move(request) });
}
inline auto texture_output_frame(const resource_registry::DescriptorBindingVersion& descriptor,
                                 VkExtent2D extent) -> GraphicsFrameData {
    GraphicsFrameData result;
    result.descriptors.push(descriptor.clone());
    GraphicsDrawBindings::Data bindings;
    bindings.viewports.push(
        { 0, float(extent.height), float(extent.width), -float(extent.height), 0, 1 });
    bindings.scissors.push({ { 0, 0 }, extent });
    result.bindings = Some(GraphicsDrawBindings(rstd::move(bindings)));
    result.draws.push({ 4, 0 });
    return result;
}
inline auto declare_graphics_surface_output(fg::FrameGraphBuilder& graph, PassId draw_id,
                                            PassId release_id, pass::ContentRef source,
                                            pass::ContentRef target, GraphicsDrawDesc draw)
    -> Result<DeclaredSurfaceOutput, GraphError> {
    auto added = graph.add_pass(draw_id);
    if (added.is_err()) return Err(added.unwrap_err());
    auto output = pass::ContentRef::write(graph, draw_id, target);
    if (output.is_err()) return Err(output.unwrap_err());
    draw.resources.textures.push({ rstd::move(source) });
    draw.resources.attachments.color =
        Some(GraphicsAttachment { .target = output->clone(), .load = AttachmentLoad::Discard });
    auto input = declare_input(graph, draw_id, rstd::move(draw));
    if (input.is_err()) return Err(input.unwrap_err());
    auto release =
        declare_input(graph, release_id, SurfaceReleaseDesc { output->clone(), PassStages::Color });
    if (release.is_err()) return Err(release.unwrap_err());
    return Ok(DeclaredSurfaceOutput { input.unwrap(), release.unwrap(), output->version() });
}
class TextureOutput {
    struct Prepared {
        Arc<resource_registry::RenderResourceSystem> resources;
        resource::DescriptorBindingHandle            descriptor;
        ~Prepared() { resources->RemovePreparedDescriptor(descriptor); }
        Prepared(Arc<resource_registry::RenderResourceSystem> owner,
                 resource::DescriptorBindingHandle            binding)
            : resources(rstd::move(owner)), descriptor(binding) {}
    };
    Option<resource::TextureUseHandle>                   source_;
    VkFormat                                             format_ { VK_FORMAT_UNDEFINED };
    Option<resource::PipelineUseHandle>                  pipeline_;
    Option<resource::RenderPassUseHandle>                render_pass_;
    Option<Arc<resource_registry::RenderResourceSystem>> resources_;
    Option<Arc<Prepared>>                                prepared_;

public:
    void configure(Option<resource::TextureUseHandle> source, VkFormat format) {
        clear();
        source_      = source;
        format_      = format;
        pipeline_    = None();
        render_pass_ = None();
    }
    void graphics_resources(resource::PipelineUseHandle   pipeline,
                            resource::RenderPassUseHandle pass) {
        clear();
        pipeline_    = Some(pipeline);
        render_pass_ = Some(pass);
    }
    auto source() const -> Option<resource::TextureUseHandle> { return source_; }
    auto pipeline() const -> Option<resource::PipelineUseHandle> { return pipeline_; }
    auto render_pass() const -> Option<resource::RenderPassUseHandle> { return render_pass_; }
    auto descriptor() const -> Option<resource::DescriptorBindingHandle> {
        return prepared_ ? Some((*prepared_)->descriptor)
                         : None<resource::DescriptorBindingHandle>();
    }
    auto graphics() const -> bool { return format_ != VK_FORMAT_UNDEFINED; }
    void clear() {
        prepared_  = None();
        resources_ = None();
    }
    auto prepare(Arc<resource_registry::RenderResourceSystem> resources,
                 const PipelineLayoutAssignments&             layouts)
        -> Result<empty, resource::ResourceError> {
        auto missing = [] {
            return Err(
                resource::ResourceError { .kind    = resource::ResourceErrorKind::MissingDefinition,
                                          .message = "texture output resources unavailable"_Str });
        };
        if (! source_ || ! resources->Prepared().Resolve(*source_)) return missing();
        if (! graphics()) {
            resources_ = Some(rstd::move(resources));
            return Ok(empty {});
        }
        if (! pipeline_ || ! render_pass_) return missing();
        auto layout = layouts.Resolve(*pipeline_);
        if (! layout) return missing();
        auto source = resources->Prepared().Resolve(*source_).unwrap();
        Vec<resource_registry::DescriptorImageBinding> images;
        images.push({ .binding = 1,
                      .image   = source->image.getActive(),
                      .layout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
        auto descriptor =
            resources->PrepareDescriptor(resources->TextureContext().session().device(),
                                         *layout,
                                         u32(1),
                                         images.as_slice(),
                                         {},
                                         resource_registry::DescriptorBindingReuse::Exclusive);
        if (descriptor.is_err()) return Err(descriptor.unwrap_err());
        resources_ = Some(resources.clone());
        prepared_  = Some(Arc<Prepared>::make(rstd::move(resources), descriptor.unwrap()));
        return Ok(empty {});
    }
    auto capture(const CompiledGraph& compiled, ResourceId logical_surface, PassId output,
                 PassId release, SurfaceTarget target, Arc<GraphicsScopeCache> scopes) const
        -> Result<GraphFrameRequest, GraphExecutionError> {
        const auto& lease   = target.lease();
        auto        invalid = [] {
            return Err(GraphExecutionError { GraphExecutionStage::Binding });
        };
        if (! source_ ||
            (graphics() && (! prepared_ || lease.format != format_ || ! lease.image.view)))
            return invalid();
        if (! resources_ ||
            target.session().epoch() != (*resources_)->TextureContext().session().epoch())
            return invalid();
        auto& resources = **resources_;
        scopes->collect();
        GraphFrameRequest request {
            .bindings = compiled.make_bindings(),
            .scopes   = Box<dyn<GraphicsScopeBackend>>::make(
                SurfaceScopeBackend(logical_surface, lease, target.owner(), rstd::move(scopes)))
        };
        auto source = resources.Prepared().Resolve(*source_);
        request.bindings.use_physical_layout(resources.Prepared().PhysicalBindings());
        if (! source) return invalid();
        auto image_slot = (*source)->image.active_slot();
        auto selected   = (*source)->TextureAt(image_slot);
        if (selected.is_err()) return invalid();
        auto surface_binding = Arc<dyn<GraphBinding>>::make(
            SurfaceGraphBinding(target.session(), lease, target.owner()));
        // SurfaceGraphBinding creates a new texture identity for this lease.
        auto surface_physical = request.bindings.add_unique_physical(
            { false, surface_binding->Info().identity.resource });
        auto source_physical = (*source)->PhysicalBinding(image_slot);
        auto source_binding = Arc<dyn<GraphBinding>>::make(GraphTextureBinding(selected->view));
        const PassId output_passes[] { output, release };
        for (auto id : output_passes) {
            auto found = compiled.description(id);
            if (! found) return invalid();
            const auto& desc = **found;
            auto        slot = compiled.description().frame_graph().pass_slot(id);
            for (const auto& use : compiled.description().passes()[usize(slot)].uses) {
                if (request.bindings.resolve(use.binding)) continue;
                bool surface = use.resource == logical_surface;
                auto bound   = request.bindings.bind(use.binding,
                                                     surface ? surface_binding.clone()
                                                             : source_binding.clone(),
                                                     surface ? surface_physical : source_physical);
                if (bound.is_err()) return Err(bound.unwrap_err());
            }
            if (desc.execution().id == output && graphics()) {
                auto sampled = sampled_image(selected->view, selected->sampler);
                if (sampled.is_err()) return invalid();
                Vec<resource_registry::DescriptorImageBinding> images;
                images.push({ .binding = 1,
                              .image   = sampled->image,
                              .layout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
                auto updated =
                    resources.UpdateDescriptorImages((*prepared_)->descriptor, images.as_slice());
                if (updated.is_err()) return invalid();
                auto descriptor = resources.Prepared().Resolve((*prepared_)->descriptor);
                if (! descriptor) return invalid();
                auto frame = texture_output_frame(
                    **descriptor, { lease.image.extent.width, lease.image.extent.height });
                frame.textures.push(texture::TextureViewRef(selected->view));
                frame.samplers.push(texture::SamplerRef(selected->sampler));
                auto bound = compiled.frame<GraphicsDrawPass>(
                    desc.execution().id, desc.revision(), rstd::move(frame));
                if (bound.is_err()) return Err(bound.unwrap_err());
                request.parameters.push(bound.unwrap());
            } else {
                auto bound = compiled.frame<SurfacePass>(
                    desc.execution().id, desc.revision(), SurfaceFrameData { lease });
                if (bound.is_err()) return Err(bound.unwrap_err());
                request.parameters.push(bound.unwrap());
            }
        }
        return Ok(rstd::move(request));
    }
};
} // namespace vrento::vulkan
