#pragma once

/**
 * @brief lifetime contract every component implements
 *
 * initialize: acquire resources and subscribe. false excludes this component only
 * start: start timers and connect. false is logged and the others continue
 * stop: stop timers, unsubscribe, release. called in reverse registration order
 */
class IComponent {
public:
    virtual ~IComponent() = default;

    virtual const char* get_name() const = 0;
    virtual bool initialize() = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
};
