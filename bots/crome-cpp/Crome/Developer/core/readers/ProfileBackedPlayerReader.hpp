#pragma once
#include "core/readers/IReaders.hpp"
#include "core/runtime/IMemory.hpp"
#include "core/runtime/RuntimeContext.hpp"
namespace crome {
class ProfileBackedPlayerReader final : public IPlayerReader {
    IMemory& mem_; RuntimeContext& rt_;
    static bool readNumber(const IMemory&, uintptr_t, const std::string&, double&);
public:
    ProfileBackedPlayerReader(IMemory& m, RuntimeContext& r):mem_(m),rt_(r){}
    PlayerSnapshot read() override;
};
}
