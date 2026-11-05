#pragma once
#include <cstddef>
#include <cstdint>
namespace crome { class IMemory { public: virtual ~IMemory()=default; virtual bool read(uintptr_t address, void* out, size_t size) const=0; virtual bool write(uintptr_t address,const void* data,size_t size)=0; template<class T> bool readT(uintptr_t a,T& out)const{return read(a,&out,sizeof(T));} }; }
