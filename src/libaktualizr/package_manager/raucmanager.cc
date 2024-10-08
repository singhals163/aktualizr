#include "raucmanager.h"
#include <fstream>
#include <iostream>
#include <stdexcept>



/*
TODO:
1. If post-install-handler script fails, it returns an error.
2. print the expected root hash on some file. (done)
3. We should handle the exception and change the bootloder config in the post-install-handler script itself. (Difficult to manage the bootloader from the aktualizr) 
    - Not possible because we can't send another request to rauc while the previous one is not served, therefore can't call `rauc status mark-bad` from post-install-handler
    - **solution, call rauc status mark-active from aktualizr in case of error** (done)
4. Implement the post-install-handler (done)
5. Handle the error separately saying no reboot is required in this case. 
6. 

*/



// Constructor: Initializes RAUC proxy and registers signal handlers
RaucManager::RaucManager(PackageConfig pconfig, const BootloaderConfig& bconfig, std::shared_ptr<INvStorage> storage,
                         std::shared_ptr<HttpInterface> http)
    : PackageManagerInterface(std::move(pconfig), bconfig, std::move(storage), std::move(http)) {
  this->installResult = data::ResultCode::Numeric::kUnknown;
  this->installResultDes = std::string("");
  const char* serviceName = "de.pengutronix.rauc";
  const char* objectPath = "/";

  // Create a proxy to interact with RAUC over D-Bus
  this->raucProxy_ = sdbus::createProxy(serviceName, objectPath);

  // Register signal handlers
  this->raucProxy_->uponSignal("Completed")
      .onInterface("de.pengutronix.rauc.Installer")
      .call([this](const std::int32_t& status) { this->onCompleted(result); });

  this->raucProxy_->uponSignal("PropertiesChanged")
      .onInterface("org.freedesktop.DBus.Properties")
      .call([this](const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                   std::vector<std::string> invalidProperties) {
        this->onProgressChanged(interfaceName, changedProperties, invalidProperties);
      });

  // Optional: Ensure we start listening to the bus
  this->raucProxy_->finishRegistration();
}

void RaucManager::handleRaucResponse(data::ResultCode resultCode) {
    installResult = resultCode;
    if(installResult == data::ResultCode::Numeric::kNeedCompletion) {
      installResultDes = "Installation Completed Successfully, restart required";
    }
    else if (installResult == data::ResultCode::Numeric::kInstallFailed) {
      // make rauc status mark-active call
      while(!installationErrorLogged) {
        sleep(1);
      }
      installResultDes = installationError;
      try {
            std::string state = "active";
            std::string slot_identifier = "booted";  // Could also be "other" or a specific slot identifier
            std::string slot_name;  // This will be filled by the call (output)
            std::string message;  // This will be filled by the call (output)

            // Call the `Mark` method
            raucProxy_->callMethod("Mark").onInterface("de.pengutronix.rauc.Installer")
                .withArguments(state, slot_identifier)
                .storeResultsTo(slot_name, message);  // Capture the output parameters
            
            std::cout << "Mark-active call successful: " << message << std::endl;
            std::cout << "Activated slot: " << slot_name << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "Error calling RAUC Mark method: " << e.what() << std::endl;
        }
    }
    installationComplete.store(true);
    return;
}

// Signal handler for "Completed" event
void RaucManager::onCompleted(const std::int32_t& status) {
  if (status == 0) {
    std::cout << "Installation completed successfully with status code: " << status << std::endl;
    handleRaucResponse(data::ResultCode::Numeric::kNeedCompletion);
  } else {
    std::cout << "Installation did not complete successfully with status code: " << status << std::endl;
    handleRaucResponse(data::ResultCode::Numeric::kInstallFailed);
  }
  return;
}

// Signal handler for "PropertiesChanged" event (progress updates)
void RaucManager::onProgressChanged(const std::string& interfaceName,
                                    const std::map<std::string, sdbus::Variant>& changedProperties,
                                    std::vector<std::string> invalidProperties) {
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
      std::cerr << "Last Error: " << lastError << std::endl;
      installationError = lastError;
      installationErrorLogged.store(true);
    }
  }
}

// Send a RAUC install request over DBus
void RaucManager::sendRaucInstallRequest(const std::string& bundlePath) const {
  auto method = this->raucProxy_->createMethodCall("de.pengutronix.rauc.Installer", "InstallBundle");
  method << bundlePath;

  try {
    installationComplete.store(false);
    installationErrorLogged.store(false);
    this->raucProxy_->callMethod(method);
  } catch (const sdbus::Error& e) {
    throw std::runtime_error("Failed to send RAUC install request: " + e.getMessage());
  }
}

Json::Value RaucManager::getInstalledPackages() const {
  std::string packages_str = Utils::readFile(config.packages_file);
  std::vector<std::string> package_lines;
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  boost::split(package_lines, packages_str, boost::is_any_of("\n"));
  Json::Value packages(Json::arrayValue);
  for (auto it = package_lines.begin(); it != package_lines.end(); ++it) {
    if (it->empty()) {
      continue;
    }
    size_t pos = it->find(" ");
    if (pos == std::string::npos) {
      throw std::runtime_error("Wrong packages file format");
    }
    Json::Value package;
    package["name"] = it->substr(0, pos);
    package["version"] = it->substr(pos + 1);
    packages.append(package);
  }
  return packages;
}

std::string getCurrentHash() {
  std::string hash = "83d05b9198e383da6d9934aac72678935afae6a046f4adf99d386975cb9d1d69";
  return hash;
}

Uptane::Target RaucManager::getCurrent() const {
  const std::string current_hash = getCurrentHash();
  boost::optional<Uptane::Target> current_version;
  // This may appear Primary-specific, but since Secondaries only know about
  // themselves, this actually works just fine for them, too.
  storage_->loadPrimaryInstalledVersions(&current_version, nullptr);

  if (!!current_version && current_version->sha256Hash() == current_hash) {
    return *current_version;
  }

  // LOG_ERROR << "Current versions in storage and reported by OSTree do not match";

  // Look into installation log to find a possible candidate. Again, despite the
  // name, this will work for Secondaries as well.
  std::vector<Uptane::Target> installed_versions;
  storage_->loadPrimaryInstallationLog(&installed_versions, false);

  // Version should be in installed versions. It's possible that multiple
  // targets could have the same sha256Hash. In this case the safest assumption
  // is that the most recent (the reverse of the vector) target is what we
  // should return.
  std::vector<Uptane::Target>::reverse_iterator it;
  for (it = installed_versions.rbegin(); it != installed_versions.rend(); it++) {
    if (it->sha256Hash() == current_hash) {
      return *it;
    }
  }
  // We haven't found a matching target. This can occur when a device is
  // freshly manufactured and the factory image is in a delegated target.
  // Aktualizr will have had no reason to fetch the relevant delegation, and it
  // doesn't know where in the delegation tree on the server it might be.
  // See https://github.com/uptane/aktualizr/issues/1 for more details. In this
  // case attempt to construct an approximate Uptane target. By getting the
  // hash correct the server has a chance to figure out what is running on the
  // device.
  Uptane::EcuMap ecus;
  std::vector<Hash> hashes{Hash(Hash::Type::kSha256, current_hash)};
  return {"unknown", ecus, hashes, 0, "RAUC"};
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
void writeHashToFile(const std::string& hash)
{
    const std::string directoryPath = "/run/aktualizr";
    const std::string filePath = directoryPath + "/expected-digest";

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


// TODO: implement error handling
// Install a target using RAUC
data::InstallationResult RaucManager::install(const Uptane::Target& target) {
    // Extract bundle URI from the target object
    std::string bundlePath = target.uri();
    // if(bundlePath == std::string::empty) {
    //   return data::InstallationResult(data::ResultCode::Numeric::kGeneralError, "Failed to get the bundle path from target");
    // }
    
    // Extract SHA256 hash from the target object
    std::string sha256Hash = target.custom()["rauc"]["rawHashes"]["sha256"].asString();  // Assuming Uptane::Target has this structure
    if(!sha256Hash) {
      return data::InstallationResult(data::ResultCode::Numeric::kGeneralError, "Failed to get the root sha256 hash from target");
    }

    // Write the SHA256 hash to the expected file
    try {
        writeHashToFile(sha256Hash);
    } catch (const std::exception& e) {
        std::cerr << "Error writing hash to file: " << e.what() << std::endl;
        return data::InstallationResult(data::ResultCode::Numeric::kGeneralError, "Failed to write SHA256 hash to file");
    }

    // Send RAUC installation request
    try {
        sendRaucInstallRequest(bundlePath);
        std::cout << "Installation request sent for bundle: " << bundlePath << std::endl;
    } catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
        return data::InstallationResult(data::ResultCode::Numeric::kGeneralError, "Failed to send RAUC installation request");
    }

    // Monitor RAUC signals for the result
    // Wait for the 'Completed' signal
    while (!installationComplete.load()) {
        sleep(1);
    }

    return data::InstallationResult(this->installResult, this->installResultDes);  // The actual result will come from the signal handlers
}

void OstreeManager::completeInstall() const {
  // LOG_INFO << "About to reboot the system in order to apply pending updates...";
  bootloader_->reboot();
}

// Finalize the installation
data::InstallationResult RaucManager::finalizeInstall(const Uptane::Target& target) {
  if (!bootloader_->rebootDetected()) {
    return data::InstallationResult(data::ResultCode::Numeric::kNeedCompletion,
                                    "Reboot is required for the pending update application");
  }

  // LOG_INFO << "Checking installation of new OSTree sysroot";
  const std::string current_hash = getCurrentHash();

  data::InstallationResult install_result =
      data::InstallationResult(data::ResultCode::Numeric::kOk, "Successfully booted on new version");

  if (current_hash != target.sha256Hash()) {
    // LOG_ERROR << "Expected to boot " << target.sha256Hash() << " but found " << current_hash
    //           << ". The system may have been rolled back.";
    install_result = data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "Wrong version booted");
  }

  bootloader_->rebootFlagClear();
  return install_result;
}

// Fetch target using Uptane Fetcher (stub implementation)
bool RaucManager::fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                              const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) {
  // Implement Uptane fetch logic if necessary
  return true;
}

// Verify the target (stub implementation)
TargetStatus RaucManager::verifyTarget(const Uptane::Target& target) const {
  // Implement verification logic based on RAUC metadata
  return TargetStatus::kGood;
}