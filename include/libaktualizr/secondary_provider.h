#ifndef UPTANE_SECONDARY_PROVIDER_H
#define UPTANE_SECONDARY_PROVIDER_H

#include <string>

#include "libaktualizr/config.h"
#include "libaktualizr/packagemanagerinterface.h"
#include "libaktualizr/types.h"

class INvStorage;

class SecondaryProviderBuilder;

class SecondaryProvider {
 public:
  friend class SecondaryProviderBuilder;

  bool getMetadata(Uptane::MetaBundle* meta_bundle, const Uptane::Target& target) const;
  bool getDirectorMetadata(Uptane::MetaBundle* meta_bundle) const;
  bool getImageRepoMetadata(Uptane::MetaBundle* meta_bundle, const Uptane::Target& target) const;
  std::string getTreehubCredentials() const;
  std::ifstream getTargetFileHandle(const Uptane::Target& target) const;

 private:
  SecondaryProvider(Config& config_in, std::shared_ptr<const INvStorage> storage_in,
                    std::shared_ptr<const PackageManagerInterface> package_manager_in)
      : config_(config_in), storage_(std::move(storage_in)), package_manager_(std::move(package_manager_in)) {}

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  Config& config_;
  std::shared_ptr<const INvStorage> storage_;
  std::shared_ptr<const PackageManagerInterface> package_manager_;
};

#endif  // UPTANE_SECONDARY_PROVIDER_H
