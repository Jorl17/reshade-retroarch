#include "backend.h"

// Not written yet: needs the Vulkan headers (Khronos Vulkan-Headers) in third_party/.
namespace
{
class Vulkan : public Backend
{
public:
    bool init(const Options &, std::string &error, bool &unsupported) override
    {
        error = "the Vulkan host is not written yet";
        unsupported = true;
        return false;
    }
    bool resize(UINT, UINT, std::string &) override { return false; }
    bool draw(const Image *, std::string &) override { return false; }
    bool read_back(Image &, std::string &) override { return false; }
    bool present(std::string &) override { return false; }
};
}

std::unique_ptr<Backend> make_vulkan()
{
    return std::make_unique<Vulkan>();
}
