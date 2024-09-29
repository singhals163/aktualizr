#include "raucmanager.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

// Atomic flag to indicate whether the installation is complete
std::atomic<bool> installationComplete(false);

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
    this->installResult = resultCode;
    installationComplete.store(true);
    return;
}

// Signal handler for "Completed" event
void RaucManager::onCompleted(const std::int32_t& status) {
  if (status == 0) {
    std::cout << "Installation completed successfully with status code: " << status << std::endl;
    this->handleRaucResponse(data::ResultCode::Numeric::kNeedCompletion);
  } else {
    std::cout << "Installation did not complete successfully with status code: " << status << std::endl;
    this->handleRaucResponse(data::ResultCode::Numeric::kInstallFailed);
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
      std::cout << "Last Error: " << lastError << std::endl;
    }
  }
}

// Send a RAUC install request over DBus
void RaucManager::sendRaucInstallRequest(const std::string& bundlePath) const {
  auto method = this->raucProxy_->createMethodCall("de.pengutronix.rauc.Installer", "InstallBundle");
  method << bundlePath;

  try {
    this->raucProxy_->callMethod(method);
  } catch (const sdbus::Error& e) {
    throw std::runtime_error("Failed to send RAUC install request: " + e.getMessage());
  }
}

// Get installed packages (stub implementation, as RAUC does not track package metadata directly)
Json::Value RaucManager::getInstalledPackages() const {
  Json::Value result;
  // Implement logic for retrieving installed packages if applicable
  return result;
}

// Get the current target (stub implementation)
Uptane::Target RaucManager::getCurrent() const {
  Uptane::Target currentTarget;
  // Implement logic for retrieving current target
  return currentTarget;
}

// Install a target using RAUC
data::InstallationResult RaucManager::install(const Uptane::Target& target) {
  // Extract bundle URI from the target object
  std::string bundlePath = target.uri;

  // Send RAUC installation request
  try {
    sendRaucInstallRequest(bundlePath);
    std::cout << "Installation request sent for bundle: " << bundlePath << std::endl;
  } catch (const std::runtime_error& e) {
    std::cerr << e.what() << std::endl;
    this->handleRaucResponse(data::ResultCode::Numeric::kGeneralError);
  }
  // Monitor RAUC signals for the result

  // Wait for the 'Completed' signal
    while (!installationComplete.load()) {
        sleep(1);
    }

  return data::InstallationResult(this->installResult, this->installResultDes);  // The actual result will come from the signal handlers
}

// Finalize the installation
data::InstallationResult RaucManager::finalizeInstall(const Uptane::Target& target) {
  // Finalization logic for RAUC installations (can be customized based on RAUC API)
  return data::InstallationResult(data::ResultCode::Numeric::kOk, this->installResultDes);
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

// Check available disk space
bool RaucManager::checkAvailableDiskSpace(uint64_t required_bytes) const {
  // Implement logic to check disk space
  return true;
}

// Create a new target file
std::ofstream RaucManager::createTargetFile(const Uptane::Target& target) {
  // Logic to create a new file for writing the target
  std::ofstream file(target.filename, std::ios::out | std::ios::trunc);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to create target file: " + target.filename);
  }
  return file;
}

// Append to an existing target file
std::ofstream RaucManager::appendTargetFile(const Uptane::Target& target) {
  std::ofstream file(target.filename, std::ios::out | std::ios::app);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to append to target file: " + target.filename);
  }
  return file;
}

// Open an existing target file
std::ifstream RaucManager::openTargetFile(const Uptane::Target& target) const {
  std::ifstream file(target.filename, std::ios::in);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open target file: " + target.filename);
  }
  return file;
}

// Remove the target file
void RaucManager::removeTargetFile(const Uptane::Target& target) {
  if (remove(target.filename.c_str()) != 0) {
    throw std::runtime_error("Failed to remove target file: " + target.filename);
  }
}

// Send a RAUC install request over DBus
void RaucManager::sendRaucInstallRequest(const std::string& bundlePath) const {
  auto method = this->raucProxy_->createMethodCall("de.pengutronix.rauc.Installer", "InstallBundle");
  method << bundlePath;

  try {
    this->raucProxy_->callMethod(method);
  } catch (const sdbus::Error& e) {
    throw std::runtime_error("Failed to send RAUC install request: " + e.getMessage());
  }
}