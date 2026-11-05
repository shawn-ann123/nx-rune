#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <foxglove/schemas.hpp>
#include "time/time.hpp"

namespace foxglove_viz
{
class FoxgloveServer;

template <typename... Ts>
class DataPublisher final
{
public:
    using Ptr = std::shared_ptr<DataPublisher<Ts...>>;

    DataPublisher(FoxgloveServer &, std::string_view, std::vector<std::string>) {}

    template <typename... Us>
    bool publish(Us &&...) { return false; }

    template <typename... Us>
    bool publish_with_time(timetool::Timestamp, Us &&...) { return false; }
};

class EntityPublisher final
{
public:
    using Ptr = std::shared_ptr<EntityPublisher>;

    bool publish_spheres(std::span<const foxglove::schemas::SpherePrimitive>) { return false; }
    bool publish_arrows(std::span<const foxglove::schemas::ArrowPrimitive>) { return false; }
};

class FoxgloveServer final
{
public:
    std::shared_ptr<EntityPublisher> create_entity_publisher(
        std::string_view, std::string_view, std::string_view)
    {
        return std::make_shared<EntityPublisher>();
    }
};

inline FoxgloveServer &global_foxglove_server()
{
    static FoxgloveServer server;
    return server;
}
} // namespace foxglove_viz
