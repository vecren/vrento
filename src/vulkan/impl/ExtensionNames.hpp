#pragma once

namespace vrento::vulkan::detail
{
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::BTreeSet;
using rstd::ffi::CString;

static auto SelectSwapchainMaintenance(bool requested, bool required, bool dependencies,
                                       bool feature) -> Result<bool, String> {
    if (! requested) return Ok(false);
    if (dependencies && feature) return Ok(true);
    if (required)
        return Err(String::make(dependencies ? "swapchainMaintenance1"_str
                                             : "VK_EXT_swapchain_maintenance1 dependencies"_str));
    return Ok(false);
}

static auto SelectExtensionNames(const BTreeSet<String>& supported, slice<Extension> requested)
    -> Result<Vec<CString>, String> {
    auto selected = BTreeSet<String>::make();
    for (const auto& extension : requested) {
        if (supported.contains(extension.name)) {
            selected.insert(String::make(extension.name));
        } else if (extension.required) {
            return Err(String::make(extension.name));
        }
    }
    auto names = Vec<CString>::with_capacity(selected.len());
    auto iterator = selected.iter();
    for (auto name : rstd::iter::for_range(iterator)) {
        auto encoded = CString::make(Vec<u8>::from(name->as_str().as_bytes()));
        if (encoded.is_err()) return Err(name->clone());
        names.push(rstd::move(encoded).unwrap());
    }
    return Ok(rstd::move(names));
}

static auto SelectInstanceExtensionNames(const BTreeSet<String>& supported,
                                         slice<Extension> requested)
    -> Result<Vec<CString>, String> {
    auto selected = SelectExtensionNames(supported, requested);
    if (selected.is_err()) return selected;
    bool surface = false, capabilities2 = false, maintenance = false;
    bool maintenance_required = false, capabilities2_required = false;
    for (const auto& extension : requested) {
        if (extension.name == "VK_EXT_surface_maintenance1"_str && extension.required)
            maintenance_required = true;
        if (extension.name == "VK_KHR_get_surface_capabilities2"_str && extension.required)
            capabilities2_required = true;
    }
    for (const auto& name : *selected) {
        auto value = name.as_ref().to_str().unwrap();
        surface |= value == "VK_KHR_surface"_str;
        capabilities2 |= value == "VK_KHR_get_surface_capabilities2"_str;
        maintenance |= value == "VK_EXT_surface_maintenance1"_str;
    }
    if (capabilities2 && ! surface) {
        if (capabilities2_required)
            return Err("VK_KHR_get_surface_capabilities2 dependencies"_Str);
        selected->retain([](const auto& name) {
            return name.as_ref().to_str().unwrap() != "VK_KHR_get_surface_capabilities2"_str;
        });
        capabilities2 = false;
    }
    if (maintenance && (! surface || ! capabilities2)) {
        if (maintenance_required) return Err("VK_EXT_surface_maintenance1 dependencies"_Str);
        selected->retain([](const auto& name) {
            return name.as_ref().to_str().unwrap() != "VK_EXT_surface_maintenance1"_str;
        });
    }
    return selected;
}

} // namespace vrento::vulkan::detail
