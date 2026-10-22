#pragma once
// crash2-pc's stand-in for SwanStation's save states, for its SPU: SPU::DoState compiles, unused
#include <cstddef>

class StateWrapper
{
public:
  template<typename T>
  void Do(T* value) { (void)value; }
  template<typename T>
  void DoArray(T* values, size_t count) { (void)values; (void)count; }
  template<typename T>
  void DoPOD(T* value) { (void)value; }
  template<typename T>
  void DoEx(T* value, unsigned version, T default_value) { (void)value; (void)version; (void)default_value; }
  void DoBytes(void* data, size_t bytes) { (void)data; (void)bytes; }
  bool IsReading() const { return false; }
  bool HasError() const { return false; }
};
