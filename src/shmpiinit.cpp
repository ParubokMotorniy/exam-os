#include "shmpiinit.h"

void SHMPI::ProcessProgramArgs(int argc, char *argv[], std::unique_ptr<SHMPI::ShmpiInstance> &instanceToInit)
{
    if (argc < 3)
    {
        throw std::invalid_argument{"Insufficient number of arguments for MPI."};
    }

    int processRank{-1};
    std::filesystem::path configPath;

    try
    {
        processRank = std::stoll(argv[1]); // may throw as well

        if (processRank < 0)
            throw;

        configPath = std::filesystem::path{argv[2]};

        if (!std::filesystem::exists(configPath))
            throw;
    }
    catch (const std::exception &parseError)
    {
        throw std::invalid_argument{"Failed to process SHMPI arguments!"};
    }

    std::string configLine;
    std::ifstream configReader{};
    configReader.open(configPath);

    if (!configReader.is_open())
        throw std::runtime_error{"Failed to open the config file!"};

    try
    {
        std::getline(configReader, configLine);
        int interfaceType = std::stoi(configLine);

        std::getline(configReader, configLine);
        int nProcessesInvolved = std::stoi(configLine);

        if (interfaceType < 0 || interfaceType > 1 || nProcessesInvolved < 0)
            throw;

        SHMPI::GlobalExecutionData processesDescription{};
        processesDescription.nProcesses = static_cast<size_t>(nProcessesInvolved);

        if (interfaceType == 0)
        {
            std::getline(configReader, configLine);
            processesDescription.sharedMemoryName.emplace(std::move(configLine));
        }
        else
        {
            processesDescription.hosts.emplace(std::vector<RemoteHost>{});
            size_t processCounter{0};
            while (true)
            {
                std::getline(configReader, configLine);

                if (configLine.empty())
                    throw;

                processesDescription.hosts.value().emplace_back(RemoteHost{.processRank = processCounter, .processAddressV4 = std::move(configLine)});
                configLine.clear(); // just to be sure

                ++processCounter;

                if (processCounter == nProcessesInvolved)
                    break;
            }
        }

        instanceToInit.reset();

        ShmpiInstance *newInstance = new ShmpiInstance(processRank, std::move(processesDescription));
        instanceToInit = std::unique_ptr<ShmpiInstance>{newInstance};
    }
    catch (const std::exception &parseError)
    {
        throw std::runtime_error{"Failed to parse the config file!"};
    }
}
