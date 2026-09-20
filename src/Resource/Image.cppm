export module vrento.image;
import rstd;
import rstd.cppstd;
export import vrento.texture_types;

export namespace vrento
{

enum class ImageKind
{
    Pixels,
    Video
};

using ImageDataPtr = std::unique_ptr<rstd::uint8_t, std::function<void(rstd::uint8_t*)>>;

struct ImageData {
    rstd::int32_t                     width {};
    rstd::int32_t                     height {};
    rstd::isize                       size {};
    ImageDataPtr                      data {};
    rstd::Option<rstd::io::ReadRange> video_source;
};

struct ImageHeader {
    ImageKind     kind { ImageKind::Pixels };
    TextureFormat format { TextureFormat::RGBA8 };
    TextureSample sample;
};

struct Image {
    Image()                        = default;
    Image(const Image&)            = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&&)                 = delete;
    Image& operator=(Image&&)      = delete;

    struct Slot {
        rstd::int32_t          width {};
        rstd::int32_t          height {};
        std::vector<ImageData> mipmaps;

        explicit operator bool() const { return width > 0 && height > 0 && ! mipmaps.empty(); }
    };

    ImageHeader       header;
    std::vector<Slot> slots;
    std::string       key;
};

} // namespace vrento
