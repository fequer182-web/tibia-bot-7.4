#pragma once
#include "core/actions/GameActions.hpp"
#include "core/readers/IReaders.hpp"
namespace crome {
class CromeApi {
    IPlayerReader& player_; ICreatureReader& creatures_; IContainerReader& containers_; IMapReader& map_; IGameActions& actions_;
public:
    CromeApi(IPlayerReader& p,ICreatureReader& c,IContainerReader& co,IMapReader& m,IGameActions& a):player_(p),creatures_(c),containers_(co),map_(m),actions_(a){}
    PlayerSnapshot player(){return player_.read();} std::vector<CreatureSnapshot> creatures(){return creatures_.readAll();} std::vector<ContainerSnapshot> containers(){return containers_.readAll();} TileSnapshot tile(Position p){return map_.tile(p);} IGameActions& actions(){return actions_;}
};
}
