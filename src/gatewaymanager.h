#ifndef GATEWAYMANAGER_H_
#define GATEWAYMANAGER_H_

#include <vector>
#include "commands.h"
#include "config.h"
#include "events.h"
#include "gateway.h"

class GatewayManager {
 public:
  GatewayManager(const Config &config, command::Channel *commands_channel_in);
  void processEvents(const std::shared_ptr<event::BaseEvent> &event);

 private:
  std::vector<std::shared_ptr<Gateway> > gateways;
};

#endif /* GATEWAYMANAGER_H_ */
