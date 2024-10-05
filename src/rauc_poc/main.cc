#include <sdbus-c++/sdbus-c++.h>
#include <iostream>
#include <string>
#include <map>
#include <atomic>
#include <fstream>
#include <json/json.h>
#include <sys/stat.h> // For directory creation
#include <fcntl.h>    // For file permissions
#include <unistd.h>   // For file operations

// Atomic flag to indicate whether the installation is complete
std::atomic<bool> installationComplete(false);

// Signal handler for the 'Completed' signal
void onCompleted(sdbus::Signal& signal)
{
    std::int32_t status;
    signal >> status;

    if (status == 0) {
        std::cout << "Installation completed successfully with status code: " << status << std::endl;
    } else {
        std::cout << "Installation did not complete successfully with status code: " << status << std::endl;
    }

    // Set the flag to true to indicate completion
    installationComplete.store(true);
}

// Signal handler for the 'PropertiesChanged' signal to monitor progress
void onProgressChanged(sdbus::Signal& signal)
{
    std::string interfaceName;
    std::map<std::string, sdbus::Variant> changedProperties;
    std::vector<std::string> invalidatedProperties;

    signal >> interfaceName >> changedProperties >> invalidatedProperties;

    if (interfaceName == "de.pengutronix.rauc.Installer") {
        auto it = changedProperties.find("Progress");
        if (it != changedProperties.end()) {
            auto progress = it->second.get<sdbus::Struct<int32_t, std::string, int32_t>>();
            auto percentage = progress.get<0>();
            auto message = progress.get<1>();
            auto depth = progress.get<2>();
            std::cout << "|";
            for (int i = 1; i < depth; i++) {
                std::cout << "  |";
            }
            std::cout << "-\"" << message << "\" (" << percentage << "%)\n";
        }

        auto itError = changedProperties.find("LastError");
        if (itError != changedProperties.end()) {
            std::string lastError = itError->second.get<std::string>();
            std::cout << "Last Error: " << lastError << std::endl;
        }
    }
}

// Function to read and parse the JSON file using JsonCpp
Json::Value readAndParseJSON(const std::string& filePath)
{
    std::ifstream file(filePath, std::ifstream::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open the JSON file.");
    }

    Json::Value jsonData;
    Json::CharReaderBuilder readerBuilder;
    std::string errs;

    if (!Json::parseFromStream(readerBuilder, file, &jsonData, &errs)) {
        throw std::runtime_error("Failed to parse JSON: " + errs);
    }

    return jsonData;
}


// Function to create the /run/aktualizr directory if it does not exist
void createDirectoryIfNotExists(const std::string& directoryPath)
{
    struct stat st;
    if (stat(directoryPath.c_str(), &st) != 0) {
        // Directory does not exist, create it
        if (mkdir(directoryPath.c_str(), 0755) != 0) {
            throw std::runtime_error("Failed to create directory: " + directoryPath);
        }
    } else if (!S_ISDIR(st.st_mode)) {
        throw std::runtime_error(directoryPath + " exists but is not a directory");
    }
}

// Function to write the SHA256 hash to /run/aktualizr/expected-digest
void writeHashToFile(const std::string& hash, const std::string directoryPath, const std::string fileName)
{
    const std::string filePath = directoryPath + fileName;

    // Create the directory if it doesn't exist
    createDirectoryIfNotExists(directoryPath);

    // Open the file in write mode (with truncation) and proper permissions
    std::ofstream file(filePath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filePath);
    }

    // Write the hash to the file
    file << hash;

    if (!file) {
        throw std::runtime_error("Failed to write to file: " + filePath);
    }

    // Explicitly close the file
    file.close();

    std::cout << "SHA256 hash written and file closed: " << filePath << std::endl;
}


int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <json file path>" << std::endl;
        return 1;
    }

    // Read and parse the JSON file
    Json::Value jsonObject;
    try {
        jsonObject = readAndParseJSON(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "Error reading or parsing JSON file: " << e.what() << std::endl;
        return 1;
    }

    // Extract 'uri' and 'rauc.rawHashes.sha256'
    std::string uri = jsonObject["custom"]["uri"].asString();
    std::string sha256Hash = jsonObject["custom"]["rauc"]["rawHashes"]["sha256"].asString();
    std::string rootHash = jsonObject["hashes"]["sha256"].asString();

    // Write the SHA256 hash to the expected file
    try {
        writeHashToFile(sha256Hash, "/run/aktualizr", "/expected-digest");
        writeHashToFile(rootHash, "/run/aktualizr", "/root-hash");
    } catch (const std::exception& e) {
        std::cerr << "Error writing hash to file: " << e.what() << std::endl;
        return 1;
    }

    // Use the URI as the bundle path
    std::string bundlePath = uri;

    // Define the service name and object path for the RAUC service
    const char* serviceName = "de.pengutronix.rauc";
    const char* objectPath = "/";

    // Create a proxy object for the RAUC service
    auto raucProxy = sdbus::createProxy(serviceName, objectPath);

    // Register the signal handler for 'Completed' signal
    raucProxy->registerSignalHandler("de.pengutronix.rauc.Installer", "Completed", &onCompleted);
    raucProxy->registerSignalHandler("org.freedesktop.DBus.Properties", "PropertiesChanged", &onProgressChanged);

    raucProxy->finishRegistration();

    // Optional arguments (key-value pairs)
    std::map<std::string, sdbus::Variant> args;

    // Invoke the InstallBundle method with the URI
    {
        auto method = raucProxy->createMethodCall("de.pengutronix.rauc.Installer", "InstallBundle");
        method << bundlePath; // Pass the URI as the bundle path
        method << args;       // Pass the args map as the second argument

        try {
            raucProxy->callMethod(method);
            std::cout << "Installation started for bundle: " << bundlePath << std::endl;
        } catch (const sdbus::Error& e) {
            std::cerr << "Failed to start installation: " << e.getName() << " - " << e.getMessage() << std::endl;
        }
    }

    // Wait for the 'Completed' signal
    while (!installationComplete.load()) {
        sleep(1);
    }

    std::cout << "Exiting program after script execution." << std::endl;
    return 0;
}
