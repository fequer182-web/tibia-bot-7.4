#pragma once
#include <functional>
#include <queue>
#include <string>
namespace crome { struct ActionRequest { int priority{}; std::string source; std::function<void()> execute; bool operator<(const ActionRequest& o)const{return priority<o.priority;} }; class ActionScheduler { std::priority_queue<ActionRequest> q_; public: void submit(ActionRequest r); bool runOne(); size_t pending()const{return q_.size();} }; }
