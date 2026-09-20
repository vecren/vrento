module;

export module vrento.shader_compile;
export import vvk;
export import vrento.shader_types;
import rstd;
import rstd.cppstd;

using namespace rstd::prelude;

export namespace vrento::vulkan
{

// ---------- Spv.hpp ----------

struct ShaderSpv {
    std::string entry_point { "main" };
    ShaderType  stage;

    std::vector<unsigned int> spirv;
};

using Uni_ShaderSpv = Box<ShaderSpv>;

// ---------- ShaderReflect.hpp ----------

struct ShaderReflected {
    struct BlockedUniform {
        int                   block_index;
        unsigned              offset;
        usize                 size { 0 };
        usize                 num { 1 };
        ShaderScalarKind      scalar_kind { ShaderScalarKind::Unknown };
        unsigned              scalar_width {};
        unsigned              vector_components { 1 };
        unsigned              matrix_rows {};
        unsigned              matrix_columns {};
        unsigned              matrix_stride {};
        ShaderMatrixMajor     matrix_major { ShaderMatrixMajor::None };
        unsigned              array_stride {};
        std::vector<unsigned> array_dimensions;
    };
    struct Block {
        int         index;
        unsigned    size;
        std::string name;
        unsigned    set { 0 };
        unsigned    binding { 0 };

        std::map<std::string, BlockedUniform, std::less<>> member_map;
    };
    std::vector<Block> blocks;

    struct Binding {
        unsigned                     set { 0 };
        VkDescriptorSetLayoutBinding layout;
    };
    std::map<std::string, Binding, std::less<>> binding_map;

    struct Input {
        unsigned location;
        VkFormat format;
    };
    std::map<std::string, Input, std::less<>> input_location_map;
};

// ---------- ShaderComp.hpp ----------

enum class VulkanTarget : unsigned
{
    Vulkan_1_0,
    Vulkan_1_1,
    Vulkan_1_2,
    Vulkan_1_3,
};

enum class SourceLang : unsigned
{
    Glsl,
    Hlsl,
};

struct ShaderCompUnit {
    ShaderType  stage;
    std::string src;
    std::string entry_point; // if empty, "main" (GLSL) or "main_<stage>" (HLSL).
    SourceLang  lang { SourceLang::Glsl };
};

struct ShaderCompOpt {
    VulkanTarget target { VulkanTarget::Vulkan_1_1 };
    bool         optimize { false };
};

struct ShaderBackend {
    using Trait                  = ShaderBackend;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ShaderBackend;

        // On failure, callers discard the output arguments.
        bool Preprocess(std::string_view src, ShaderType stage, SourceLang lang,
                        std::string& out) const {
            return rstd::trait_call<0>(this, src, stage, lang, out);
        }

        bool CompileAndLinkShaderUnits(std::span<const ShaderCompUnit> units,
                                       const ShaderCompOpt&            opt,
                                       std::vector<Uni_ShaderSpv>&     spvs) const {
            return rstd::trait_call<1>(this, units, opt, spvs);
        }

        bool GenReflect(std::span<const std::vector<unsigned int>> codes,
                        std::vector<Uni_ShaderSpv>& spvs, ShaderReflected& reflected) const {
            return rstd::trait_call<2>(this, codes, spvs, reflected);
        }
    };

    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::Preprocess, &T::CompileAndLinkShaderUnits, &T::GenReflect>;
};

} // namespace vrento::vulkan
