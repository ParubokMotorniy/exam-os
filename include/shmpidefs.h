#ifndef SHMPI_H
#define SHMPI_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <concepts>
#include <unistd.h>
#include <cstring>
#include <iostream>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>

namespace SHMPI
{
    class ShmpiInstance;

    void ProcessProgramArgs(int argc, char *argv[], std::unique_ptr<SHMPI::ShmpiInstance> &instanceToInit);
}

namespace SHMPI
{
    template <typename T>
    concept IsShmpiTransmittable = std::integral<T> || std::floating_point<T>;

    struct RemoteHost
    {
        size_t processRank;
        std::string processAddressV4;
    };

    struct OtherProcesses
    {
        size_t nProcesses;
        std::optional<std::vector<RemoteHost>> hosts;
        std::optional<std::string> sharedMemoryName;
    };

    enum class CommunicationType
    {
        SHMPI_LOCAL,
        SHMPI_NETWORK,
    };

    constexpr size_t SHARED_MEMORY_BUFFER_SIZE = 4096; // in bytes

    struct SharedMemoryProcessSegment
    {
        size_t ownerProcessRank{0};                           // who owns this segment and wait for data to be written
        sem_t bufferAccessMutex;                              // controls the writes to the buffer
        unsigned char writeBuffer[SHARED_MEMORY_BUFFER_SIZE]; // where other processes will write their stuff

        size_t lastAccessorRank{0}; // the rank of process who accessed the segment most recently top write some data
        size_t bytesWritten{0};
    };

    class ShmpiInstance
    {
        friend void SHMPI::ProcessProgramArgs(int argc, char *argv[], std::unique_ptr<SHMPI::ShmpiInstance> &instanceToInit);

    public:
        ShmpiInstance &operator=(const ShmpiInstance &) = delete;
        ShmpiInstance &operator=(ShmpiInstance &&) = delete;

        ShmpiInstance(const ShmpiInstance &) = delete;
        ShmpiInstance(ShmpiInstance &&) = delete;

        ~ShmpiInstance()
        {
            std::cout << "Terminating " << myRank << std::endl;

            SynchronizeOnBarrier(shmpiTerminationSemaphore);

            if (comType == CommunicationType::SHMPI_LOCAL)
            {
                munmap(mappedSegments, sizeof(SharedMemoryProcessSegment) * otherProcesses.nProcesses);
                sem_close(shmpiBarrierSemaphore);

                if (myRank == 0)
                {
                    shm_unlink(otherProcesses.sharedMemoryName.value().c_str());

                    sem_destroy(shmpiBarrierSemaphore);
                    sem_destroy(shmpiTerminationSemaphore);
                }

                close(sharedFd);
            }
        }

        [[nodiscard]] size_t getRank() const { return myRank; }
        [[nodiscard]] size_t getGlobalNumberOfOutputs() const { return otherProcesses.nProcesses; }

        template <typename T>
            requires IsShmpiTransmittable<T> // returns 0 on success
        int SHMPI_Send(const T *buf, int count, size_t dest)
        {
            if (dest >= otherProcesses.nProcesses)
            {
                return -1;
            }

            if (comType == CommunicationType::SHMPI_LOCAL)
            {
                if (sizeof(T) * count > SHARED_MEMORY_BUFFER_SIZE)
                {
                    return -1;
                }

                SendToLocal(buf, count, dest);
                return 0;
            }

            return -1;
        }

        template <typename T>
            requires IsShmpiTransmittable<T> //returns the number of bytes read
        int SHMPI_Recv(T *buf, int count, size_t source, int readTimeOut = -1)
        {
            if (source >= otherProcesses.nProcesses)
            {
                return 0;
            }

            if (comType == CommunicationType::SHMPI_LOCAL)
            {
                if (sizeof(T) * count > SHARED_MEMORY_BUFFER_SIZE)
                {
                    return 0;
                }

                return ReadFromLocal(buf, count, source, readTimeOut);
            }

            return 0;
        }

        template <typename T>
            requires IsShmpiTransmittable<T>
        int SHMPI_Isend(const T *buf, int count, int dest);

        template <typename T>
            requires IsShmpiTransmittable<T>
        int SHMPI_Irecv(T *buf, int count, int source);

    private:
        ShmpiInstance(size_t processRank, OtherProcesses &&hosts) : myRank(processRank), otherProcesses(std::move(hosts))
        {
            if (otherProcesses.sharedMemoryName.has_value())
                comType = CommunicationType::SHMPI_LOCAL;
            else
                comType = CommunicationType::SHMPI_NETWORK;

            // initialization
            if (comType == CommunicationType::SHMPI_LOCAL)
            {
                if (myRank == 0)
                {
                    shm_unlink(otherProcesses.sharedMemoryName.value().c_str());
                    unlink(otherProcesses.sharedMemoryName.value().c_str());

                    // initialize shared memory and mutexes for other procs to use
                    sharedFd = shm_open(otherProcesses.sharedMemoryName.value().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXG | S_IRWXU);
                    if (sharedFd < 0)
                        throw std::runtime_error{"Failed to create the shared memory file!"};

                    if (ftruncate(sharedFd, sizeof(SharedMemoryProcessSegment) * otherProcesses.nProcesses))
                        throw std::runtime_error{"Failed to truncate the shared memory file to the required length!"};

                    void *sharedMemoryPtr;
                    if ((sharedMemoryPtr = mmap(nullptr, sizeof(SharedMemoryProcessSegment) * otherProcesses.nProcesses, PROT_READ | PROT_WRITE, MAP_SHARED, sharedFd, 0)) == MAP_FAILED)
                        throw std::runtime_error{"Failed to map the shared memory file for the main process!"};

                    mappedSegments = static_cast<SharedMemoryProcessSegment *>(sharedMemoryPtr);

                    for (size_t i = 0; i < otherProcesses.nProcesses; ++i)
                    {
                        (mappedSegments + i)->ownerProcessRank = i;
                        (mappedSegments + i)->lastAccessorRank = -1;
                        memset((mappedSegments + i)->writeBuffer, SHARED_MEMORY_BUFFER_SIZE, 0);

                        if (sem_init(&(mappedSegments + i)->bufferAccessMutex, 1, 1), 0)
                            throw std::runtime_error{"Failed to initialize a mutex!"};
                    }
                }
                else
                {
                    while ((sharedFd = shm_open(otherProcesses.sharedMemoryName.value().c_str(), O_RDWR, 660)) < 0)
                    {
                    } // TODO: add some better termination criteria

                    void *sharedMemoryPtr;
                    if ((sharedMemoryPtr = mmap(nullptr, sizeof(SharedMemoryProcessSegment) * otherProcesses.nProcesses, PROT_READ | PROT_WRITE, MAP_SHARED, sharedFd, 0)) == MAP_FAILED)
                        throw std::runtime_error{"Failed to map the shared memory file for a slave process!"};

                    mappedSegments = static_cast<SharedMemoryProcessSegment *>(sharedMemoryPtr);
                }

                if ((shmpiBarrierSemaphore = sem_open(shmpiBarrierName.c_str(), O_CREAT | O_RDWR, S_IRWXG | S_IRWXU, otherProcesses.nProcesses)) == SEM_FAILED)
                    throw std::runtime_error{"Failed to open a synchronization semaphore!"};

                if ((shmpiTerminationSemaphore = sem_open(shmpiTerminationBarrierName.c_str(), O_CREAT | O_RDWR, S_IRWXG | S_IRWXU, otherProcesses.nProcesses)) == SEM_FAILED)
                    throw std::runtime_error{"Failed to open a termination semaphore!"};
            }
        }

        template <typename T>
            requires IsShmpiTransmittable<T>
        void SendToLocal(const T *buf, int count, size_t dest)
        {
            SharedMemoryProcessSegment *targetSegment = mappedSegments + dest;
            sem_wait(&(targetSegment->bufferAccessMutex));

            memcpy(targetSegment->writeBuffer, buf, sizeof(T) * count);
            targetSegment->lastAccessorRank = myRank;
            targetSegment->bytesWritten = sizeof(T) * count;

            sem_post(&(targetSegment->bufferAccessMutex));

            std::cout << "Sent data from " << myRank << " to " << dest << std::endl;
        }

        template <typename T>
            requires IsShmpiTransmittable<T>
        size_t ReadFromLocal(T *buf, size_t count, size_t source, int readTimeOut = -1)
        {
            if (count == 0)
                return 0;

            SharedMemoryProcessSegment *mySegment = mappedSegments + myRank;

            auto readProcedure = [=]() -> size_t
            {      
                sem_wait(&(mySegment->bufferAccessMutex));

                if (mySegment->lastAccessorRank == source) // TODO: probably think of something more efficient
                {
                    size_t numBytesToRead = std::min(SHARED_MEMORY_BUFFER_SIZE, sizeof(T) * count);

                    memcpy(buf, mySegment->writeBuffer, numBytesToRead);

                    return numBytesToRead;
                }

                sem_post(&(mySegment->bufferAccessMutex));

                return 0;
            };

            size_t nRead{0};
            if(readTimeOut == -1)
            {
                while(nRead == 0)
                {
                    nRead = readProcedure();
                }
            }else
            {   
                while(readTimeOut > 0)
                {
                    nRead = readProcedure();
                    --readTimeOut;
                }

            }

            std::cout << "Received data from " << source << " . Me " << myRank << std::endl;

            return nRead;
        }

        void SynchronizeOnBarrier(sem_t *semPtr)
        {
            sem_wait(semPtr);

            int semValue{otherProcesses.nProcesses};
            while (semValue != 0)
            {
                sem_getvalue(semPtr, &semValue);
            }

            sem_post(semPtr);
        }

    private:
        OtherProcesses otherProcesses;
        size_t myRank{0};
        CommunicationType comType;

        int sharedFd{-1};
        SharedMemoryProcessSegment *mappedSegments{};

        const std::string shmpiBarrierName{"_shmpi_barrier_"};
        const std::string shmpiTerminationBarrierName{"_shmpi_termination_"};
        
        sem_t *shmpiBarrierSemaphore{};
        sem_t *shmpiTerminationSemaphore{};
    };

}

#endif