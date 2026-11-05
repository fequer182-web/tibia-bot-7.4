#pragma once
#include "core/sdk/Snapshots.hpp"
namespace crome {
class IPlayerReader { public: virtual ~IPlayerReader()=default; virtual PlayerSnapshot read()=0; };
class ICreatureReader { public: virtual ~ICreatureReader()=default; virtual std::vector<CreatureSnapshot> readAll()=0; };
class IContainerReader { public: virtual ~IContainerReader()=default; virtual std::vector<ContainerSnapshot> readAll()=0; };
class IMapReader { public: virtual ~IMapReader()=default; virtual TileSnapshot tile(Position)=0; };
}
