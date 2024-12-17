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
#include <thread>
#include <future>

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

    enum class CommunicationType
    {
        SHMPI_LOCAL,
        SHMPI_NETWORK,
    };

    struct GlobalExecutionData
    {
        size_t nProcesses;
        std::optional<std::vector<RemoteHost>> hosts;
        std::optional<std::string> sharedMemoryName;
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
                munmap(mappedSegments, sizeof(SharedMemoryProcessSegment) * executionData.nProcesses);
                sem_close(shmpiBarrierSemaphore);

                if (myRank == 0)
                {
                    shm_unlink(executionData.sharedMemoryName.value().c_str());

                    sem_destroy(shmpiBarrierSemaphore);
                    sem_destroy(shmpiTerminationSemaphore);
                }

                close(sharedFd);
            }

            delete[] sendingFlags;
        }

        [[nodiscard]] size_t getRank() const { return myRank; }
        [[nodiscard]] size_t getGlobalNumberOfOutputs() const { return executionData.nProcesses; }

        template <typename T>
            requires IsShmpiTransmittable<T> // returns 0 on success
        int SHMPI_Send(const T *buf, int count, size_t dest)
        {
            if (dest >= executionData.nProcesses)
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
            requires IsShmpiTransmittable<T> // returns the number of bytes read
        int SHMPI_Read(T *buf, int count, size_t source, int readTimeOut = -1)
        {
            if (source >= executionData.nProcesses)
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
        void SHMPI_ISend(const T *buf, int count, int dest, std::future<int> &result);

        template <typename T>
            requires IsShmpiTransmittable<T>
        void SHMPI_IRead(const T *buf, int count, int dest, std::future<size_t> &result);

    private:
        ShmpiInstance(size_t processRank, GlobalExecutionData &&hosts) : myRank(processRank), executionData(std::move(hosts))
        {
            iAmWaitingForDataAsync.clear();
            sendingFlags = new std::atomic_flag[executionData.nProcesses];

            if (executionData.sharedMemoryName.has_value())
                comType = CommunicationType::SHMPI_LOCAL;
            else
                comType = CommunicationType::SHMPI_NETWORK;

            // initialization
            if (comType == CommunicationType::SHMPI_LOCAL)
            {
                if (myRank == 0)
                {
                    shm_unlink(executionData.sharedMemoryName.value().c_str());
                    unlink(executionData.sharedMemoryName.value().c_str());

                    // initialize shared memory and mutexes for other procs to use
                    sharedFd = shm_open(executionData.sharedMemoryName.value().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXG | S_IRWXU);
                    if (sharedFd < 0)
                        throw std::runtime_error{"Failed to create the shared memory file!"};

                    if (ftruncate(sharedFd, sizeof(SharedMemoryProcessSegment) * executionData.nProcesses))
                        throw std::runtime_error{"Failed to truncate the shared memory file to the required length!"};

                    void *sharedMemoryPtr;
                    if ((sharedMemoryPtr = mmap(nullptr, sizeof(SharedMemoryProcessSegment) * executionData.nProcesses, PROT_READ | PROT_WRITE, MAP_SHARED, sharedFd, 0)) == MAP_FAILED)
                        throw std::runtime_error{"Failed to map the shared memory file for the main process!"};

                    mappedSegments = static_cast<SharedMemoryProcessSegment *>(sharedMemoryPtr);

                    for (size_t i = 0; i < executionData.nProcesses; ++i)
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
                    while ((sharedFd = shm_open(executionData.sharedMemoryName.value().c_str(), O_RDWR, 660)) < 0)
                    {
                    } // TODO: add some better termination criteria

                    void *sharedMemoryPtr;
                    if ((sharedMemoryPtr = mmap(nullptr, sizeof(SharedMemoryProcessSegment) * executionData.nProcesses, PROT_READ | PROT_WRITE, MAP_SHARED, sharedFd, 0)) == MAP_FAILED)
                        throw std::runtime_error{"Failed to map the shared memory file for a slave process!"};

                    mappedSegments = static_cast<SharedMemoryProcessSegment *>(sharedMemoryPtr);
                }

                if ((shmpiBarrierSemaphore = sem_open(shmpiBarrierName.c_str(), O_CREAT | O_RDWR, S_IRWXG | S_IRWXU, executionData.nProcesses)) == SEM_FAILED)
                    throw std::runtime_error{"Failed to open a synchronization semaphore!"};

                if ((shmpiTerminationSemaphore = sem_open(shmpiTerminationBarrierName.c_str(), O_CREAT | O_RDWR, S_IRWXG | S_IRWXU, executionData.nProcesses)) == SEM_FAILED)
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
            if (readTimeOut == -1)
            {
                while (nRead == 0)
                {
                    nRead = readProcedure();
                }
            }
            else
            {
                while (readTimeOut > 0)
                {
                    nRead = readProcedure();
                    --readTimeOut;
                }
            }

            std::cout << "Received data from " << source << " . Me " << myRank << std::endl;
            return nRead;
        }

        template <typename T>
            requires IsShmpiTransmittable<T>
        void IReadFromLocal(T *buf, size_t count, size_t source, std::atomic_flag &flagToClear, std::promise<size_t> promise, int readTimeOut = -1)
        {
            size_t result = ReadFromLocal(buf, count, source, readTimeOut);
            promise.set_value(result);
            flagToClear.clear();
        }

        template <typename T>
            requires IsShmpiTransmittable<T>
        void ISendToLocal(const T *buf, int count, size_t dest, std::atomic_flag &flagToClear, std::promise<int> promise)
        {
            int result = SendToLocal(buf, count, dest);
            promise.set_value(result);
            flagToClear.clear();
        }

        void SynchronizeOnBarrier(sem_t *semPtr)
        {
            sem_wait(semPtr);

            int semValue{executionData.nProcesses};
            while (semValue != 0)
            {
                sem_getvalue(semPtr, &semValue);
            }

            sem_post(semPtr);
        }

    private:
        GlobalExecutionData executionData;
        size_t myRank{0};
        CommunicationType comType;

        int sharedFd{-1};
        SharedMemoryProcessSegment *mappedSegments{};

        const std::string shmpiBarrierName{"_shmpi_barrier_"};
        const std::string shmpiTerminationBarrierName{"_shmpi_termination_"};

        sem_t *shmpiBarrierSemaphore{};
        sem_t *shmpiTerminationSemaphore{};

        std::atomic_flag iAmWaitingForDataAsync;
        std::atomic_flag *sendingFlags; // we can not be sending to the same process at the same time
    };

    template <typename T>
        requires IsShmpiTransmittable<T>
    void ShmpiInstance::SHMPI_ISend(const T *buf, int count, int dest, std::future<int> &result)
    {
        if (dest >= executionData.nProcesses || (sendingFlags + dest)->test())
        {
            return;
        }

        if (comType == CommunicationType::SHMPI_LOCAL)
        {
            if (sizeof(T) * count > SHARED_MEMORY_BUFFER_SIZE || count == 0)
            {
                return;
            }

            std::promise<int> sendResultPromise{};
            result = sendResultPromise.get_future();
            (sendingFlags + dest)->test_and_set();
            
            std::thread sender{ShmpiInstance::ISendToLocal, this, buf, count, dest, std::ref(*(sendingFlags + dest)), std::move(sendResultPromise)};

            return;
        }

        return;
    }

    template <typename T>
        requires IsShmpiTransmittable<T>
    void ShmpiInstance::SHMPI_IRead(const T *buf, int count, int dest, std::future<size_t> &result)
    {
        if (dest >= executionData.nProcesses || iAmWaitingForDataAsync.test()) // performing two asynchronous reads currently is not possible
        {
            return;
        }

        if (comType == CommunicationType::SHMPI_LOCAL)
        {
            if (sizeof(T) * count > SHARED_MEMORY_BUFFER_SIZE || count = 0)
            {
                return;
            }

            std::promise<size_t> readResultPromise{};
            result = readResultPromise.get_future();
            iAmWaitingForDataAsync.test_and_set();

            std::thread sender{ShmpiInstance::IReadFromLocal, this, buf, count, std::ref(iAmWaitingForDataAsync), std::move(readResultPromise), dest};

            return;
        }

        return;
    }
}

#endif