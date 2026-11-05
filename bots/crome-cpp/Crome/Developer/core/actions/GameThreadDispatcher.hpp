#pragma once
#include <functional>
namespace crome { class IGameThreadDispatcher { public: virtual ~IGameThreadDispatcher()=default; virtual bool dispatch(std::function<void()> fn)=0; }; }
