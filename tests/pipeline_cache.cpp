#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
import rstd;
import rstd.test;
import vrento;
import vrento.tests.shaders;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace vrento::vulkan;
using namespace vrento::resource_registry;
using rstd::sync::Arc;

namespace vrento_test::pipeline_cache
{

template<rstd::size_t N>
auto stage(const unsigned (&words)[N], vrento::ShaderType type) -> Uni_ShaderSpv {
    auto value   = Box<ShaderSpv>::make();
    value->stage = type;
    value->spirv = vrento::ShaderCode::from(slice<rstd::uint32_t>::from_raw_parts(words, usize(N)));
    return value;
}
auto request(vrento::resource::PipelineLayoutHandle layout) -> PipelineResourceRequest {
    GraphicsPipeline defaults;
    defaults.toDefault();
    PipelineResourceRequest value;
    value.pipeline_layout            = layout;
    value.topology                   = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    value.depth                      = defaults.depth;
    value.raster                     = defaults.raster;
    value.multisample                = defaults.multisample;
    value.color_blend.colorWriteMask = 15;
    value.shader_stages.push(stage(shaders::vertex.words, vrento::ShaderType::VERTEX));
    value.shader_stages.push(stage(shaders::fragment.words, vrento::ShaderType::FRAGMENT));
    return value;
}
TEST(PipelineCache, CompatibilityKey) {
    auto original                 = request({ u64(1), u64(1) });
    auto key                      = MakePipelineCacheKey(original);
    original.color_load_op        = VK_ATTACHMENT_LOAD_OP_CLEAR;
    original.color_initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    original.color_final_layout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    rstd_assert(SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    auto render_key        = MakeRenderPassCacheKey(original);
    original.color_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
    rstd_assert(! SameRenderPassCacheKey(render_key, MakeRenderPassCacheKey(original)));
    rstd_assert(SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    original.color_format = VK_FORMAT_R8G8B8A8_SRGB;
    rstd_assert(! SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    original.color_format                     = VK_FORMAT_R8G8B8A8_UNORM;
    original.multisample.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    rstd_assert(! SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    original.multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    original.raster.cullMode                  = VK_CULL_MODE_FRONT_BIT;
    rstd_assert(! SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
}
TEST(PipelineCache, FailureIsTakenOnceAndDiagnosticRemains) {
    PipelineBuild build({},
                        Arc<vvk::RenderPass>::make(),
                        Arc<PipelineLayoutResourceEntry>::make(),
                        Arc<vvk::PipelineCache>::make());
    EXPECT_FALSE(build.take_error());
    build.fail(
        vrento::resource::ResourceError { .message = "pipeline failure"_Str,
                                          .source  = Some(vrento::resource::ResourceErrorSource::Io(
                                              rstd::io::Error::from_raw_os_error(i32(5)))) });
    EXPECT_EQ(build.status(), PipelineReadiness::Failed);
    auto diagnostic = build.error();
    ASSERT_TRUE(diagnostic);
    EXPECT_TRUE(build.error());
    auto owned = build.take_error();
    ASSERT_TRUE(owned);
    ASSERT_TRUE(owned->source->is_Io());
    EXPECT_EQ(owned->source->as_Io().source.raw_os_error().unwrap(), i32(5));
    EXPECT_FALSE(build.take_error());
    EXPECT_EQ(build.status(), PipelineReadiness::Failed);
    build.cancel();
    build.fail(vrento::resource::ResourceError { .message = "late failure"_Str });
    EXPECT_FALSE(build.take_error());
    owned = None();
    EXPECT_TRUE(build.error()->diagnostic.message.as_str().contains("pipeline failure"_str));
    EXPECT_FALSE(diagnostic->diagnostic.sources.is_empty());
}
TEST(PipelineCache, CancelledBuildKeepsFailedStatusAfterDelivery) {
    PipelineBuild build({},
                        Arc<vvk::RenderPass>::make(),
                        Arc<PipelineLayoutResourceEntry>::make(),
                        Arc<vvk::PipelineCache>::make());
    build.cancel();
    auto owned = build.take_error();
    ASSERT_TRUE(owned);
    EXPECT_EQ(owned->kind, vrento::resource::ResourceErrorKind::Cancelled);
    EXPECT_EQ(build.status(), PipelineReadiness::Failed);
    build.cancel();
    EXPECT_FALSE(build.take_error());
    EXPECT_EQ(build.error()->kind, vrento::resource::ResourceErrorKind::Cancelled);
}
TEST(PipelineCache, ClonesShareCodeAndKeysCompareFullContent) {
    auto        original  = request({ u64(1), u64(1) });
    auto        key       = MakePipelineCacheKey(original);
    auto        cloned    = original.clone();
    auto        desc      = MakePipelineResourceDesc(cloned);
    auto        desc_copy = desc.clone();
    const auto* words     = original.shader_stages[usize()]->spirv.data();
    EXPECT_EQ(cloned.shader_stages[usize()]->spirv.data(), words);
    EXPECT_EQ(desc.shader_stages[usize()].spirv.data(), words);
    EXPECT_EQ(desc_copy.shader_stages[usize()].spirv.data(), words);
    EXPECT_TRUE(SamePipelineCacheKey(key, MakePipelineCacheKey(cloned)));
    auto independent = request({ u64(1), u64(1) });
    EXPECT_NE(independent.shader_stages[usize()]->spirv.data(), words);
    EXPECT_TRUE(SamePipelineCacheKey(key, MakePipelineCacheKey(independent)));
    independent.shader_stages[usize()]->entry_point = "other"_Str;
    auto changed                                    = MakePipelineCacheKey(independent);
    changed.value                                   = key.value;
    EXPECT_FALSE(SamePipelineCacheKey(key, changed));
    original.shader_stages.clear();
    cloned.shader_stages.clear();
    desc.shader_stages.clear();
    EXPECT_EQ(desc_copy.shader_stages[usize()].spirv.data(), words);
    EXPECT_EQ(desc_copy.shader_stages[usize()].spirv[usize()], 0x07230203u);
    EXPECT_TRUE(SamePipelineCacheKey(key, MakePipelineCacheKey(desc_copy)));
}

TEST(PipelineCache, KeyConstructionTimings) {
    auto                original = request({ u64(1), u64(1) });
    Vec<rstd::uint32_t> words;
    words.resize(usize(16384), 0x07230203u);
    auto code = vrento::ShaderCode::from(rstd::move(words));
    for (auto& shader : original.shader_stages) shader->spirv = code.clone();
    auto               desc    = MakePipelineResourceDesc(original);
    constexpr unsigned samples = 128;
    usize              checksum {}, key_bytes {};
    const auto         started = rstd::time::Instant::now();
    for (unsigned sample {}; sample < samples; ++sample) {
        auto key = MakePipelineCacheKey(desc);
        checksum ^= usize(key.value);
        key_bytes = key.bytes.len();
    }
    rstd::io::println("pipeline key: {} shared stages, {} words/stage, {} queries, {:.3} ms, "
                      "{} key bytes, checksum {}",
                      desc.shader_stages.len(),
                      code.len(),
                      samples,
                      started.elapsed().as_secs_f64() * 1000,
                      key_bytes,
                      checksum);
    EXPECT_EQ(checksum, usize());
}

TEST(PipelineCache, ShaderKeysStayCompactAndRetainCode) {
    auto                original = request({ u64(1), u64(1) });
    auto                small    = MakePipelineCacheKey(original);
    Vec<rstd::uint32_t> words;
    words.resize(usize(16384), 0x07230203u);
    original.shader_stages[usize()]->spirv = vrento::ShaderCode::from(rstd::move(words));
    const auto* pointer                    = original.shader_stages[usize()]->spirv.data();
    auto        large                      = MakePipelineCacheKey(original);
    EXPECT_EQ(small.bytes.len(), large.bytes.len());
    EXPECT_LT(large.bytes.len(), usize(1024));
    ASSERT_EQ(large.shader_codes.len(), usize(2));
    EXPECT_EQ(large.shader_codes[usize()].data(), pointer);
    EXPECT_FALSE(SamePipelineCacheKey(small, large));
    auto retained = large.clone();
    auto state    = MakePipelineStateKey(MakePipelineResourceDesc(original));
    auto variant  = MakePipelineVariantKey(state, MakeRenderPassResourceDesc(original));
    EXPECT_TRUE(SamePipelineCacheKey(large, variant));
    original.shader_stages.clear();
    large = {};
    state = {};
    EXPECT_EQ(retained.shader_codes[usize()].data(), pointer);
    EXPECT_EQ(variant.shader_codes[usize()].data(), pointer);
    EXPECT_EQ(retained.shader_codes[usize()][usize(16383)], 0x07230203u);
    EXPECT_TRUE(SamePipelineCacheKey(retained, variant));
}

TEST(PipelineCache, EqualHashAndMetadataStillCheckShaderContent) {
    auto original = request({ u64(1), u64(1) });
    auto first    = MakePipelineCacheKey(original);
    auto words    = Vec<rstd::uint32_t>::from(original.shader_stages[usize()]->spirv.as_slice());
    words[words.len() - usize(1)] ^= 1u;
    original.shader_stages[usize()]->spirv = vrento::ShaderCode::from(rstd::move(words));
    auto different                         = MakePipelineCacheKey(original);
    different.value                        = first.value;
    different.bytes                        = first.bytes.clone();
    EXPECT_FALSE(SamePipelineCacheKey(first, different));
    auto clone = different.clone();
    EXPECT_FALSE(SamePipelineCacheKey(first, clone));
    clone.clone_from(first);
    EXPECT_TRUE(SamePipelineCacheKey(first, clone));
    auto pass = MakeRenderPassResourceDesc(original);
    EXPECT_FALSE(SamePipelineCacheKey(MakePipelineVariantKey(first, pass),
                                      MakePipelineVariantKey(different, pass)));
    rstd::collections::
        HashMap<PipelineCacheKey, u32, rstd::hash::RandomState, PipelineCacheKeyEqual>
            entries;
    (void)entries.insert(first.clone(), u32(1));
    (void)entries.insert(different.clone(), u32(2));
    EXPECT_EQ(entries.len(), usize(2));
    EXPECT_EQ(*entries.get(first).unwrap(), u32(1));
    EXPECT_EQ(*entries.get(different).unwrap(), u32(2));
}

TEST(PipelineCache, ShaderMetadataAndCodeRemainLiveInputs) {
    auto original = request({ u64(1), u64(1) });
    auto key      = MakePipelineCacheKey(original);
    rstd::mem::swap(original.shader_stages[usize()], original.shader_stages[usize(1)]);
    EXPECT_TRUE(SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    original.shader_stages[usize()]->entry_point = "other"_Str;
    EXPECT_FALSE(SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    original.shader_stages[usize()]->entry_point = "main"_Str;
    original.shader_stages[usize()]->stage       = vrento::ShaderType::GEOMETRY;
    EXPECT_FALSE(SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    original.shader_stages[usize()]->stage = vrento::ShaderType::FRAGMENT;
    EXPECT_TRUE(SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    original.shader_stages[usize()]->spirv = {};
    EXPECT_FALSE(SamePipelineCacheKey(key, MakePipelineCacheKey(original)));
    auto empty                             = MakePipelineCacheKey(original);
    original.shader_stages[usize()]->spirv = vrento::ShaderCode::from(Vec<rstd::uint32_t> {});
    EXPECT_TRUE(SamePipelineCacheKey(empty, MakePipelineCacheKey(original)));
}

TEST(PipelineCache, RetentionAndRenderPass) {
    auto                     session = DeviceSession::create({ .validation = true }).unwrap();
    DescriptorLayoutRegistry descriptors;
    PipelineLayoutRegistry   layouts;
    RenderPassRegistry       passes;
    PipelineRegistry         pipelines;
    auto                     layout = layouts.Ensure(session.device(), {}, descriptors).unwrap();
    auto                     first =
        Some(pipelines.Ensure(session.device(), request(layout.handle), passes, layouts).unwrap());
    auto first_handle          = first->handle;
    auto changed               = request(layout.handle);
    changed.color_load_op      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    changed.color_final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    {
        auto second =
            pipelines.Ensure(session.device(), rstd::move(changed), passes, layouts).unwrap();
        rstd_assert(second.cache_hit && second.handle == first->handle);
        rstd_assert(second.render_pass != first->render_pass);
        rstd_assert(! SameRenderPassCacheKey(first->render_pass_key, second.render_pass_key));
    }
    rstd_assert(pipelines.stats().creations == u64(1));
    pipelines.SetLimits({ .max_entries = usize(), .unused_collections = u64() });
    rstd_assert(pipelines.Resolve(first_handle).is_some());
    first = None();
    pipelines.PruneExpired();
    rstd_assert(pipelines.entryCount() == usize());
    rstd_assert(pipelines.Resolve(first_handle).is_none());
    pipelines.SetLimits({ .max_entries = usize(8), .unused_collections = u64(10) });
    auto cached_handle = first_handle;
    {
        auto cached =
            pipelines.Ensure(session.device(), request(layout.handle), passes, layouts).unwrap();
        cached_handle = cached.handle;
    }
    pipelines.PruneExpired();
    rstd_assert(pipelines.Resolve(cached_handle).is_some());
    auto reused =
        pipelines.Ensure(session.device(), request(layout.handle), passes, layouts).unwrap();
    rstd_assert(reused.cache_hit && reused.handle == cached_handle);
    for (unsigned i {}; i < 8; ++i)
        rstd_assert(
            pipelines.Ensure(session.device(), request(layout.handle), passes, layouts)->cache_hit);
    rstd_assert(pipelines.stats().creations == u64(2));
    auto recipe     = pipelines.RegisterRecipe(request(layout.handle), layouts).unwrap();
    auto equivalent = pipelines.RegisterRecipe(request(layout.handle), layouts).unwrap();
    auto scope      = MakeRenderPassResourceDesc(request(layout.handle));
    auto hot = pipelines.EnsureRecipe(session.device(), recipe, scope, passes, layouts).unwrap();
    rstd_assert(hot.cache_hit && hot.handle == cached_handle);
    scope.color_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    rstd_assert(
        pipelines.EnsureRecipe(session.device(), equivalent, scope, passes, layouts)->cache_hit);
    rstd_assert(pipelines.stats().creations == u64(2));
    rstd_assert(pipelines.ReleaseRecipe(recipe));
    rstd_assert(pipelines.EnsureRecipe(session.device(), recipe, scope, passes, layouts).is_none());
    rstd_assert(session.validation_errors() == 0);
}
TEST(PipelineCache, RetainedStateRecipeSeparatesLayoutAndRenderPass) {
    auto  session  = DeviceSession::create({ .validation = true }).unwrap();
    auto& graphics = session.graphics_resources();
    auto& device   = session.device();
    auto  layout   = graphics.layouts.Ensure(device, {}, graphics.descriptors).unwrap();
    auto  source   = Some(Arc<PipelineStateRecipe>::make(request({})));
    auto  get      = [&](const Arc<PipelineStateRecipe>& state, auto handle) {
        return graphics.pipelines.RegisterStateRecipe(device, state, handle, graphics.layouts)
            .unwrap();
    };
    auto recipe = get(*source, layout.handle);
    EXPECT_EQ(get(*source, layout.handle), recipe);
    auto equivalent   = Arc<PipelineStateRecipe>::make(request({}));
    auto other_recipe = get(equivalent, layout.handle);
    auto signature    = MakeRenderPassResourceDesc((*source)->state());
    auto ensure       = [&](auto id) {
        return graphics.pipelines
            .EnsureRecipe(device, id, signature, graphics.passes, graphics.layouts)
            .unwrap();
    };
    auto first              = ensure(recipe);
    signature.color_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    auto same               = ensure(other_recipe);
    EXPECT_TRUE(same.cache_hit);
    EXPECT_EQ(same.handle, first.handle);
    EXPECT_NE(same.render_pass, first.render_pass);
    signature.color_format = VK_FORMAT_R8G8B8A8_SRGB;
    EXPECT_NE(ensure(recipe).handle, first.handle);
    signature.samples = VK_SAMPLE_COUNT_4_BIT;
    EXPECT_TRUE(graphics.pipelines
                    .TryEnsureRecipe(device, recipe, signature, graphics.passes, graphics.layouts)
                    .is_err());
    signature.samples                  = VK_SAMPLE_COUNT_1_BIT;
    signature.color_format             = VK_FORMAT_R8G8B8A8_UNORM;
    auto changed                       = request({});
    changed.color_blend.colorWriteMask = 7;
    auto new_state                     = Arc<PipelineStateRecipe>::make(rstd::move(changed));
    EXPECT_NE(ensure(get(new_state, layout.handle)).handle, first.handle);

    PipelineLayoutRequest with_constants;
    with_constants.push_constants.push({ VK_SHADER_STAGE_VERTEX_BIT, 0, 4 });
    auto other_layout =
        graphics.layouts.Ensure(device, with_constants, graphics.descriptors).unwrap();
    auto layout_recipe = get(*source, other_layout.handle);
    EXPECT_NE(layout_recipe, recipe);
    EXPECT_NE(ensure(layout_recipe).handle, first.handle);
    source = None();
    graphics.pipelines.PruneExpired();
    EXPECT_TRUE(graphics.pipelines
                    .TryEnsureRecipe(device, recipe, signature, graphics.passes, graphics.layouts)
                    .is_err());
    EXPECT_EQ(ensure(other_recipe).handle, first.handle);
    graphics.pipelines.Reset();
    auto new_recipe = get(equivalent, layout.handle);
    EXPECT_NE(new_recipe.generation, other_recipe.generation);
    EXPECT_EQ(session.validation_errors(), 0);
}
TEST(PipelineCache, DriverData) {
    auto  session  = DeviceSession::create({ .validation = true }).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    auto  pipeline =
        graphics.pipelines
            .Ensure(session.device(), request(layout.handle), graphics.passes, graphics.layouts)
            .unwrap();
    auto encoded = graphics.pipelines.driver_cache().encode(session.device()).unwrap();
    rstd_assert(encoded.len() >= usize(48));
    DriverPipelineCache warm;
    warm.initialize(session.device(), encoded.as_slice()).unwrap();
    rstd_assert(warm.stats().loaded && ! warm.stats().rejected);
    auto warm_data = warm.encode(session.device()).unwrap();
    rstd_assert(warm_data.len() >= usize(48));
    auto directory = rstd::fs::TempDir::make("vrento-pipeline-cache"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(directory.path().as_os_str().to_os_string());
    auto path      = root.join("driver.cache"_str);
    warm.save(session.device(), path.as_path()).unwrap();
    DriverPipelineCache loaded;
    loaded.load(session.device(), path.as_path()).unwrap();
    rstd_assert(loaded.stats().loaded);
    encoded[encoded.len() - usize(1)] =
        u8(encoded.as_slice()[encoded.len() - usize(1)].to_primitive() ^ 1);
    warm.initialize(session.device(), encoded.as_slice()).unwrap();
    rstd_assert(! warm.stats().loaded && warm.stats().rejected);
    auto truncated = slice<u8>::from_raw_parts(encoded.data(), usize(24));
    warm.initialize(session.device(), truncated).unwrap();
    rstd_assert(warm.stats().rejected);
    auto other = DeviceSession::create().unwrap();
    rstd_assert(warm.encode(other.device()).unwrap_err().stage == DriverCacheStage::Device);
    auto restored = DeviceSession::create({ .pipeline_cache_data = warm_data.as_slice() }).unwrap();
    rstd_assert(restored.graphics_resources().pipelines.driver_cache().stats().loaded);
    rstd_assert(session.validation_errors() == 0);
}
TEST(PipelineCache, RegistryOwnership) {
    auto               session = DeviceSession::create({ .validation = true }).unwrap();
    ResourceRegistries first, second;
    rstd_assert(first.Initialize(session) && second.Initialize(session));
    rstd_assert(&first.PipelineCache() == &session.graphics_resources().pipelines);
    rstd_assert(&first.PipelineLayouts() == &second.PipelineLayouts());
    auto layout =
        first.PipelineLayouts().Ensure(session.device(), {}, first.DescriptorLayouts()).unwrap();
    auto created = first.Pipelines()
                       .Ensure(session.device(),
                               request(layout.handle),
                               first.RenderPasses(),
                               first.PipelineLayouts())
                       .unwrap();
    first.Reset();
    auto reused = second.Pipelines()
                      .Ensure(session.device(),
                              request(layout.handle),
                              second.RenderPasses(),
                              second.PipelineLayouts())
                      .unwrap();
    rstd_assert(reused.cache_hit && reused.handle == created.handle);
    second.Reset();
    rstd_assert(session.graphics_resources().pipelines.stats().creations == u64(1));
    rstd_assert(session.validation_errors() == 0);
}
TEST(PipelineCache, AsyncPreparation) {
    auto  session  = DeviceSession::create({ .validation = true }).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    auto  scope    = MakeRenderPassResourceDesc(request(layout.handle));
    auto  hot_recipe =
        graphics.pipelines.RegisterRecipe(request(layout.handle), graphics.layouts).unwrap();
    auto hot =
        graphics.pipelines
            .EnsureRecipe(session.device(), hot_recipe, scope, graphics.passes, graphics.layouts)
            .unwrap();
    auto low_request            = request(layout.handle);
    low_request.raster.cullMode = VK_CULL_MODE_BACK_BIT;
    auto low_recipe =
        graphics.pipelines.RegisterRecipe(rstd::move(low_request), graphics.layouts).unwrap();
    auto high_request            = request(layout.handle);
    high_request.raster.cullMode = VK_CULL_MODE_FRONT_BIT;
    auto high_recipe =
        graphics.pipelines.RegisterRecipe(rstd::move(high_request), graphics.layouts).unwrap();
    auto preparer  = PipelinePreparer::create(session, usize(1)).unwrap();
    auto low       = preparer.request(low_recipe, scope).unwrap();
    auto high      = preparer.request(high_recipe, scope, 10).unwrap();
    auto duplicate = preparer.request(high_recipe, scope, 20).unwrap();
    rstd_assert(rstd::sync::Arc<PipelineBuild>::ptr_eq(high.build, duplicate.build));
    rstd_assert(high.build->status() == PipelineReadiness::Pending);
    rstd_assert(
        graphics.pipelines
            .EnsureRecipe(session.device(), high_recipe, scope, graphics.passes, graphics.layouts)
            .is_none());
    preparer.advance();
    rstd_assert(low.build->status() == PipelineReadiness::Pending);
    for (unsigned i {}; i < 16; ++i)
        rstd_assert(graphics.pipelines
                        .EnsureRecipe(
                            session.device(), hot_recipe, scope, graphics.passes, graphics.layouts)
                        ->handle == hot.handle);
    auto started = rstd::time::Instant::now();
    while (high.build->status() == PipelineReadiness::Pending &&
           started.elapsed().as_secs() < u64(5))
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    rstd_assert(high.build->status() == PipelineReadiness::Ready);
    preparer.close();
    rstd_assert(low.build->status() == PipelineReadiness::Failed);
    auto ready = preparer.poll(high).unwrap();
    rstd_assert(ready.status == PipelineReadiness::Ready && ready.pipeline.is_some());
    auto again = preparer.poll(duplicate).unwrap();
    rstd_assert(again.pipeline->handle == ready.pipeline->handle);
    rstd_assert(graphics.pipelines.stats().creations == u64(2));
    auto failed = preparer.poll(low).unwrap();
    EXPECT_EQ(failed.status, PipelineReadiness::Failed);
    EXPECT_FALSE(failed.error);
    ASSERT_TRUE(failed.diagnostic);
    EXPECT_EQ(failed.diagnostic->kind, vrento::resource::ResourceErrorKind::Cancelled);
    auto cancelled = preparer.take_error(low);
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(cancelled->kind, vrento::resource::ResourceErrorKind::Cancelled);
    EXPECT_FALSE(preparer.take_error(low));
    EXPECT_EQ(preparer.poll(low)->status, PipelineReadiness::Failed);
    rstd_assert(preparer.request(high_recipe, scope).unwrap_err().kind ==
                vrento::resource::ResourceErrorKind::Closed);
    auto retry   = PipelinePreparer::create(session).unwrap();
    auto retried = retry.request(low_recipe, scope, 0, true).unwrap();
    EXPECT_FALSE(rstd::sync::Arc<PipelineBuild>::ptr_eq(low.build, retried.build));
    retry.advance();
    retry.close();
    rstd_assert(retry.poll(retried)->status == PipelineReadiness::Ready);
    rstd_assert(graphics.pipelines.stats().creations == u64(3));
    rstd_assert(session.validation_errors() == 0);
}
TEST(PipelineCache, ResourcePreparationPending) {
    auto                 session = DeviceSession::create({ .validation = true }).unwrap();
    RenderResourceSystem resources;
    rstd_assert(resources.Initialize(session));
    resources.SetPipelinePreparationMode(PipelinePreparationMode::Deferred).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    vrento::resource::PipelineUseHandle   use { u64(1), u64(1) };
    vrento::resource::RenderPassUseHandle pass { u64(1), u64(1) };
    auto pending = resources.PreparePipeline(use, pass, session.device(), request(layout.handle));
    rstd_assert(pending.is_ok() && ! pending->ready());
    rstd_assert(resources.Prepared().Resolve(use).is_none());
    auto start = rstd::time::Instant::now();
    for (;;) {
        auto result =
            resources.PreparePipeline(use, pass, session.device(), request(layout.handle));
        rstd_assert(result.is_ok());
        if (result->ready()) break;
        rstd_assert(start.elapsed().as_secs() < u64(5));
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    }
    rstd_assert(resources.Prepared().Resolve(use).is_some());
    rstd_assert(graphics.pipelines.stats().creations == u64(1));
    auto repeated = resources.PreparePipeline(use, pass, session.device(), request(layout.handle));
    rstd_assert(repeated.is_ok());
    rstd_assert(repeated->take_ready().physical->pipeline.handle);
    rstd_assert(graphics.pipelines.stats().creations == u64(1));
    auto cold            = request(layout.handle);
    cold.raster.cullMode = VK_CULL_MODE_FRONT_BIT;
    vrento::resource::PipelineUseHandle   second { u64(2), u64(1) };
    vrento::resource::RenderPassUseHandle second_pass { u64(2), u64(1) };
    auto                                  cancelled =
        resources.PreparePipeline(second, second_pass, session.device(), rstd::move(cold));
    rstd_assert(cancelled.is_ok() && ! cancelled->ready());
    resources.AbortPreparePlan();
    rstd_assert(resources.Prepared().Resolve(second).is_none());
    resources.Reset();
    rstd_assert(session.validation_errors() == 0);
}
TEST(PipelineCache, RetainedStateRecipeSurvivesDeferredGraphUses) {
    auto                 session = DeviceSession::create({ .validation = true }).unwrap();
    RenderResourceSystem resources;
    ASSERT_TRUE(resources.Initialize(session));
    resources.SetPipelinePreparationMode(PipelinePreparationMode::Deferred).unwrap();
    auto& graphics  = session.graphics_resources();
    auto  layout    = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    auto  state     = Arc<PipelineStateRecipe>::make(request({}));
    auto  signature = MakeRenderPassResourceDesc(state->state());
    auto  begin     = [&](u64 index) {
        return resources.PreparePipeline({ index, u64(1) },
                                         { index, u64(1) },
                                         session.device(),
                                         state,
                                         layout.handle,
                                         signature);
    };
    ASSERT_FALSE(begin(u64(1)).unwrap().ready());
    EXPECT_EQ(resources.SetPipelinePreparationMode(PipelinePreparationMode::Synchronous)
                  .unwrap_err()
                  .kind,
              vrento::resource::ResourceErrorKind::Busy);
    auto start = rstd::time::Instant::now();
    for (;;) {
        auto prepared = begin(u64(1)).unwrap();
        if (prepared.ready()) break;
        ASSERT_LT(start.elapsed().as_secs(), u64(5));
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    }
    resources.AbortPreparePlan();
    auto warm = begin(u64(2)).unwrap().take_ready();
    EXPECT_TRUE(warm.cache_hit);
    EXPECT_EQ(graphics.pipelines.stats().creations, u64(1));
    EXPECT_EQ(session.validation_errors(), 0);
}
TEST(PipelineCache, PreparationDeviceLoss) {
    auto  session  = DeviceSession::create({ .validation = true }).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    auto  recipe =
        graphics.pipelines.RegisterRecipe(request(layout.handle), graphics.layouts).unwrap();
    auto signature = MakeRenderPassResourceDesc(request(layout.handle));
    auto worker    = PipelinePreparer::create(session).unwrap();
    auto queued    = worker.request(recipe, signature).unwrap();
    session.observe_result(VK_ERROR_DEVICE_LOST);
    rstd_assert(worker.poll(queued)->status == PipelineReadiness::Failed);
    rstd_assert(queued.build->status() == PipelineReadiness::Failed);
    rstd_assert(worker.request(recipe, signature).unwrap_err().kind ==
                vrento::resource::ResourceErrorKind::Device);
    worker.close();
    rstd_assert(graphics.pipelines.stats().creations == u64());
}
TEST(PipelineCache, CreationTimings) {
    auto  session  = DeviceSession::create({ .validation = true }).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    constexpr unsigned samples = 3, hits = 128;
    double             cold_ms {}, warm_ms {}, object_ms {};
    for (unsigned sample {}; sample < samples; ++sample) {
        graphics.pipelines.Reset();
        graphics.pipelines.driver_cache().initialize(session.device()).unwrap();
        auto started = rstd::time::Instant::now();
        {
            auto cold =
                graphics.pipelines
                    .Ensure(
                        session.device(), request(layout.handle), graphics.passes, graphics.layouts)
                    .unwrap();
            rstd_assert(! cold.cache_hit);
        }
        cold_ms += started.elapsed().as_secs_f64() * 1000;
        auto data = graphics.pipelines.driver_cache().encode(session.device()).unwrap();
        graphics.pipelines.Reset();
        graphics.pipelines.driver_cache().initialize(session.device(), data.as_slice()).unwrap();
        rstd_assert(graphics.pipelines.driver_cache().stats().loaded);
        started = rstd::time::Instant::now();
        auto warm =
            graphics.pipelines
                .Ensure(session.device(), request(layout.handle), graphics.passes, graphics.layouts)
                .unwrap();
        warm_ms += started.elapsed().as_secs_f64() * 1000;
        auto recipe =
            graphics.pipelines.RegisterRecipe(request(layout.handle), graphics.layouts).unwrap();
        auto signature                 = MakeRenderPassResourceDesc(request(layout.handle));
        signature.color_initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        auto created                   = graphics.pipelines.stats().creations;
        started                        = rstd::time::Instant::now();
        for (unsigned query {}; query < hits; ++query) {
            signature.color_load_op =
                query % 2 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            auto cached =
                graphics.pipelines
                    .EnsureRecipe(
                        session.device(), recipe, signature, graphics.passes, graphics.layouts)
                    .unwrap();
            rstd_assert(cached.cache_hit && cached.handle == warm.handle);
        }
        object_ms += started.elapsed().as_secs_f64() * 1000;
        rstd_assert(graphics.pipelines.stats().creations == created);
    }
    rstd::io::println(
        "pipeline: {} samples; cold driver {:.3} ms; loaded driver {:.3} ms; {} object hits "
        "{:.3} ms; hot creations 0",
        samples,
        cold_ms / samples,
        warm_ms / samples,
        hits,
        object_ms / samples);
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vrento_test::pipeline_cache

namespace vrento_test::pipeline_cache
{
TEST(PipelineCache, PendingUseDoesNotPublishPreviousRecipe) {
    auto created = DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto                 session = created.unwrap();
    RenderResourceSystem resources;
    ASSERT_TRUE(resources.Initialize(session));
    resources.SetPipelinePreparationMode(PipelinePreparationMode::Deferred).unwrap();
    auto& graphics = session.graphics_resources();
    auto  layout   = graphics.layouts.Ensure(session.device(), {}, graphics.descriptors).unwrap();
    auto  replacement                      = request(layout.handle);
    replacement.color_blend.colorWriteMask = 7;
    auto expected =
        graphics.pipelines
            .Ensure(session.device(), replacement.clone(), graphics.passes, graphics.layouts)
            .unwrap();
    vrento::resource::PipelineUseHandle   use { u64(1), u64(1) };
    vrento::resource::RenderPassUseHandle pass { u64(1), u64(1) };
    auto pending = resources.PreparePipeline(use, pass, session.device(), request(layout.handle));
    ASSERT_TRUE(pending.is_ok());
    EXPECT_FALSE(pending->ready());
    auto changed = resources.PreparePipeline(use, pass, session.device(), rstd::move(replacement));
    ASSERT_TRUE(changed.is_ok());
    EXPECT_EQ(changed->take_ready().resource, expected.handle);
    EXPECT_EQ(resources.Prepared().Resolve(use).unwrap()->resource, expected.handle);
    resources.Reset();
    EXPECT_EQ(session.validation_errors(), 0u);
}
} // namespace vrento_test::pipeline_cache
