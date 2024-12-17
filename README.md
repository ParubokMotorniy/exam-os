# Compilation
```
mkdir build
cd build
cmake .. -GNinja
ninja
```

# Library functionality

+ Allows to send/receive messages locally
+ A process iniitalizes its `shmpi` handle with `void ProcessProgramArgs(int argc, char *argv[], std::unique_ptr<SHMPI::ShmpiInstance> &instanceToInit)`. See `loop_tester.sh` for example
+ Afterwards, the read/write operations are executed through public interface of `ShmpiInstance` 
+ The read/write operations can be executed either synchronously or asynchronously
+ `shmpi` sounds funny
   