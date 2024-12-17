#include "shmpiinit.h"
#include "shmpidefs.h"

int main(int argc, char *argv[])
{
    std::unique_ptr<SHMPI::ShmpiInstance> myInstance;
    SHMPI::ProcessProgramArgs(argc, argv, myInstance);

    

    exit(EXIT_SUCCESS);
}