#ifndef SHMPIINIT_H
#define SHMPIINIT_H

#include "shmpidefs.h"

#include <memory>
#include <exception>
#include <filesystem>
#include <fstream>

namespace SHMPI
{
    void ProcessProgramArgs(int argc, char *argv[], std::unique_ptr<SHMPI::ShmpiInstance> &instanceToInit);
}

#endif