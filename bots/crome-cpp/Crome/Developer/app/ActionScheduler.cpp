#include "app/ActionScheduler.hpp"
namespace crome { void ActionScheduler::submit(ActionRequest r){q_.push(std::move(r));} bool ActionScheduler::runOne(){if(q_.empty())return false; auto r=q_.top(); q_.pop(); if(r.execute)r.execute(); return true;} }
