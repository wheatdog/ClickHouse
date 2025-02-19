#include <Server/ReplicasStatusHandler.h>

#include <Databases/IDatabase.h>
#include <IO/HTTPCommon.h>
#include <Interpreters/Context.h>
#include <Server/HTTP/HTMLForm.h>
#include <Server/HTTPHandlerFactory.h>
#include <Server/HTTPHandlerRequestFilter.h>
#include <Server/IServer.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Common/typeid_cast.h>

#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>


namespace DB
{

ReplicasStatusHandler::ReplicasStatusHandler(IServer & server) : WithContext(server.context())
{
}

void ReplicasStatusHandler::handleRequest(HTTPServerRequest & request, HTTPServerResponse & response, const ProfileEvents::Event & /*write_event*/)
{
    try
    {
        HTMLForm params(getContext()->getSettingsRef(), request);

        const auto & config = getContext()->getConfigRef();

        const MergeTreeSettings & settings = getContext()->getReplicatedMergeTreeSettings();

        /// Even if lag is small, output detailed information about the lag.
        bool verbose = false;
        bool enable_verbose = config.getBool("enable_verbose_replicas_status", true);

        if (params.get("verbose", "") == "1" && enable_verbose)
            verbose = true;

        bool ok = true;
        WriteBufferFromOwnString message;

        auto databases = DatabaseCatalog::instance().getDatabases();

        /// Iterate through all the replicated tables.
        for (const auto & db : databases)
        {
            /// Check if database can contain replicated tables
            if (!db.second->canContainMergeTreeTables())
                continue;

            for (auto iterator = db.second->getTablesIterator(getContext()); iterator->isValid(); iterator->next())
            {
                const auto & table = iterator->table();
                if (!table)
                    continue;

                StorageReplicatedMergeTree * table_replicated = dynamic_cast<StorageReplicatedMergeTree *>(table.get());

                if (!table_replicated)
                    continue;

                time_t absolute_delay = 0;
                time_t relative_delay = 0;

                if (!table_replicated->isTableReadOnly())
                {
                    table_replicated->getReplicaDelays(absolute_delay, relative_delay);

                    if ((settings.min_absolute_delay_to_close && absolute_delay >= static_cast<time_t>(settings.min_absolute_delay_to_close))
                        || (settings.min_relative_delay_to_close && relative_delay >= static_cast<time_t>(settings.min_relative_delay_to_close)))
                        ok = false;

                    message << backQuoteIfNeed(db.first) << "." << backQuoteIfNeed(iterator->name())
                        << ":\tAbsolute delay: " << absolute_delay << ". Relative delay: " << relative_delay << ".\n";
                }
                else
                {
                    message << backQuoteIfNeed(db.first) << "." << backQuoteIfNeed(iterator->name())
                        << ":\tis readonly. \n";
                }
            }
        }

        const auto & server_settings = getContext()->getServerSettings();
        setResponseDefaultHeaders(response, server_settings.keep_alive_timeout.totalSeconds());

        if (!ok)
        {
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_SERVICE_UNAVAILABLE);
            if (enable_verbose)
                verbose = true;
        }

        if (verbose)
            *response.send() << message.str();
        else
        {
            const char * data = "Ok.\n";
            response.sendBuffer(data, strlen(data));
        }
    }
    catch (...)
    {
        tryLogCurrentException("ReplicasStatusHandler");

        try
        {
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);

            if (!response.sent())
            {
                /// We have not sent anything yet and we don't even know if we need to compress response.
                *response.send() << getCurrentExceptionMessage(false) << '\n';
            }
        }
        catch (...)
        {
            LOG_ERROR((&Poco::Logger::get("ReplicasStatusHandler")), "Cannot send exception to client");
        }
    }
}

HTTPRequestHandlerFactoryPtr createReplicasStatusHandlerFactory(IServer & server,
    const Poco::Util::AbstractConfiguration & config,
    const std::string & config_prefix)
{
    auto factory = std::make_shared<HandlingRuleHTTPHandlerFactory<ReplicasStatusHandler>>(server);
    factory->addFiltersFromConfig(config, config_prefix);
    return factory;
}

}
