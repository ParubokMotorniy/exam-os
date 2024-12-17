#include "shmpiinit.h"
#include "shmpidefs.h"

#include <sstream>
#include <iostream>

int main(int argc, char *argv[])
{
    std::unique_ptr<SHMPI::ShmpiInstance> myInstance;
    SHMPI::ProcessProgramArgs(argc, argv, myInstance);

    const size_t myRank = myInstance->getRank();
    std::stringstream message;
    int number{-1};

    if(myRank == 0)
    {
        number = 1683;
        message << "My rank: " << myRank << ". Starting number: " << number;
    }else
    {
        myInstance->SHMPI_Recv(&number, 1, myRank - 1, -1); //no timeout
        message << "My rank: " << myRank << ". Received number: " << number;
    }

    ++number;
    std::cout << message.str() << std::endl; 
    myInstance->SHMPI_Send(&number, 1, myRank + 1); //no error anyway

    exit(EXIT_SUCCESS);
}