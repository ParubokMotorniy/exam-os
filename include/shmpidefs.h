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

        int lastAccessorRank{-1};                             // the rank of process who accessed the segment most recently top write some data
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
            if (comType == CommunicationType::SHMPI_LOCAL)
            {
                munmap(mappedSegments, sizeof(SharedMemoryProcessSegment) * otherProcesses.nProcesses);

                if (myRank == 0)
                {

                    shm_unlink(otherProcesses.sharedMemoryName.value().c_str());
                }
            }
        }

        [[nodiscard]] size_t getRank() const { return myRank; }

        template <typename T>
            requires IsShmpiTransmittable<T>
        int SHMPI_Send(const T *buf, int count, int dest);

        template <typename T>
            requires IsShmpiTransmittable<T>
        int SHMPI_Recv(T *buf, int count, int source);

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
                    unlink(otherProcesses.sharedMemoryName.value().c_str());

                    // initialize shared memory and mutexes for other procs to use
                    sharedFd = shm_open(otherProcesses.sharedMemoryName.value().c_str(), O_RDWR | O_CREAT | O_TRUNC, 660);
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

                if((shmpiBarrierSemaphore = sem_open(shmpiBarrierName.c_str(), O_CREAT, 660, otherProcesses.nProcesses)) == SEM_FAILED)
                    throw std::runtime_error{"Failed to open a semaphore!"};

                
            }
        }

        template <typename T>
            requires IsShmpiTransmittable<T>
        void SendToLocal(const T *buf, int count, size_t dest)
        {
            SharedMemoryProcessSegment *targetSegment = mappedSegments + dest;
            sem_wait(&targetSegment->bufferAccessMutex);

            memcpy(targetSegment->writeBuffer, buf, sizeof(T) * count);
            targetSegment->lastAccessorRank = myRank;
            targetSegment->bytesWritten = sizeof(T) * count;

            sem_post(&targetSegment->bufferAccessMutex);
        }

        template <typename T>
            requires IsShmpiTransmittable<T>
        size_t ReadFromLocal(T *buf, int count, size_t source)
        {
            SharedMemoryProcessSegment *sourceSegment = mappedSegments + source;

            size_t maxLockAttempts{10};
            int mostRecentWriter = -1;
            while(maxLockAttempts > 0)
            {
                sem_wait(&sourceSegment->bufferAccessMutex);   

                if(sourceSegment->lastAccessorRank == source) //TODO: probably think of something more efficient
                {
                    size_t numBytesToRead = std::min(SHARED_MEMORY_BUFFER_SIZE, sizeof(T) * count);

                    memcpy(buf, sourceSegment->writeBuffer, numBytesToRead);

                    return numBytesToRead;
                }

                sem_post(&sourceSegment->bufferAccessMutex);

                --maxLockAttempts;
            }

            return 0;
        }

        void BlockOnBarrier()
        {
            sem_wait(shmpiBarrierSemaphore);

            int semValue{otherProcesses.nProcesses};
            while(semValue != 0)
            {
                sem_getvalue(shmpiBarrierSemaphore, &semValue);
            }

            sem_post(shmpiBarrierSemaphore);
        }

    private:
        OtherProcesses otherProcesses;
        size_t myRank{0};
        CommunicationType comType;
 
        int sharedFd{-1};
        SharedMemoryProcessSegment *mappedSegments{};

        const std::string shmpiBarrierName{"shmpi_barrier"};
        sem_t *shmpiBarrierSemaphore;
    };

}

#endif