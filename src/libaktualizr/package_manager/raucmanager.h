#ifndef RAUC_H_
#define RAUC_H_

#include <boost/algorithm/string.hpp>
#include <boost/optional/optional.hpp>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <atomic>
#include "libaktualizr/packagemanagerinterface.h"
#include "utilities/utils.h"
#include "json/json.h"  
#include <sys/stat.h>
#include "bootloader/bootloader.h"
#include "libaktualizr/types.h"
#include "storage/invstorage.h"

class RaucManager : public PackageManagerInterface {
 public:
  // Constructor, using PackageManagerInterface's constructor
  RaucManager(const PackageConfig &pconfig, const BootloaderConfig &bconfig,
                const std::shared_ptr<INvStorage> &storage, const std::shared_ptr<HttpInterface> &http,
                Bootloader *bootloader = nullptr);

  // Destructor
  ~RaucManager() override = default;

  RaucManager(const RaucManager &) = delete;
  RaucManager(RaucManager &&) = delete;
  RaucManager &operator=(const RaucManager &) = delete;
  RaucManager &operator=(RaucManager &&) = delete;

  // Overriding necessary functions from PackageManagerInterface
  std::string name() const override { return "rauc"; };
  Json::Value getInstalledPackages() const override;
  Uptane::Target getCurrent() const override;
  data::InstallationResult install(const Uptane::Target& target) const override;
  void completeInstall() const override;
  data::InstallationResult finalizeInstall(const Uptane::Target& target) override;
  bool fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                   const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) override;
  TargetStatus verifyTarget(const Uptane::Target& target) const override;

 private:
  void handleRaucResponse(data::ResultCode::Numeric resultCode);
  // Signal handlers for installation progress and completion
  void onCompleted(const std::int32_t& status);
  void onProgressChanged(const std::string& interfaceName,
                         const std::map<std::string, sdbus::Variant>& changedProps,
                         std::vector<std::string> invalidProperties);

  // Method to send the installation request to RAUC via D-Bus
  void sendRaucInstallRequest(const std::string& bundlePath) const;
  // RAUC-related configurations and proxy object for DBus communication
  data::ResultCode::Numeric installResult;
  std::string installResultDes;
  std::string installationError;
  // Atomic flag to indicate whether the installation is complete
  std::atomic<bool> installationComplete;
  std::atomic<bool> installationErrorLogged;
  std::shared_ptr<sdbus::IProxy> raucProxy_;
  int result;

  std::unique_ptr<Bootloader> bootloader_;
};

#endif  // RAUC_H_
