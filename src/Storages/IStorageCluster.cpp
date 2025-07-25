#include <Storages/IStorageCluster.h>

#include <Common/Exception.h>
#include <Core/Settings.h>
#include <Core/QueryProcessingStage.h>
#include <DataTypes/DataTypeString.h>
#include <IO/ConnectionTimeouts.h>
#include <Interpreters/Cluster.h>
#include <Interpreters/Context.h>
#include <Interpreters/getHeaderForProcessingStage.h>
#include <Interpreters/SelectQueryOptions.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/AddDefaultDatabaseVisitor.h>
#include <Interpreters/TranslateQualifiedNamesVisitor.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Processors/Sources/NullSource.h>
#include <Processors/Sources/RemoteSource.h>
#include <Processors/QueryPlan/SourceStepWithFilter.h>
#include <QueryPipeline/narrowPipe.h>
#include <QueryPipeline/Pipe.h>
#include <QueryPipeline/RemoteQueryExecutor.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Storages/IStorage.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageDictionary.h>

#include <algorithm>
#include <memory>
#include <string>


namespace DB
{
namespace Setting
{
    extern const SettingsBool allow_experimental_analyzer;
    extern const SettingsBool async_query_sending_for_remote;
    extern const SettingsBool async_socket_for_remote;
    extern const SettingsBool skip_unavailable_shards;
    extern const SettingsBool parallel_replicas_local_plan;
    extern const SettingsString cluster_for_parallel_replicas;
    extern const SettingsNonZeroUInt64 max_parallel_replicas;
}

IStorageCluster::IStorageCluster(
    const String & cluster_name_,
    const StorageID & table_id_,
    LoggerPtr log_)
    : IStorage(table_id_)
    , log(log_)
    , cluster_name(cluster_name_)
{
}

class ReadFromCluster : public SourceStepWithFilter
{
public:
    std::string getName() const override { return "ReadFromCluster"; }
    void initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &) override;
    void applyFilters(ActionDAGNodes added_filter_nodes) override;

    ReadFromCluster(
        const Names & column_names_,
        const SelectQueryInfo & query_info_,
        const StorageSnapshotPtr & storage_snapshot_,
        const ContextPtr & context_,
        SharedHeader sample_block,
        std::shared_ptr<IStorageCluster> storage_,
        ASTPtr query_to_send_,
        QueryProcessingStage::Enum processed_stage_,
        ClusterPtr cluster_,
        LoggerPtr log_)
        : SourceStepWithFilter(
            std::move(sample_block),
            column_names_,
            query_info_,
            storage_snapshot_,
            context_)
        , storage(std::move(storage_))
        , query_to_send(std::move(query_to_send_))
        , processed_stage(processed_stage_)
        , cluster(std::move(cluster_))
        , log(log_)
    {
    }

private:
    std::shared_ptr<IStorageCluster> storage;
    ASTPtr query_to_send;
    QueryProcessingStage::Enum processed_stage;
    ClusterPtr cluster;
    LoggerPtr log;

    std::optional<RemoteQueryExecutor::Extension> extension;

    void createExtension(const ActionsDAG::Node * predicate, size_t number_of_replicas);
    ContextPtr updateSettings(const Settings & settings);
};

void ReadFromCluster::applyFilters(ActionDAGNodes added_filter_nodes)
{
    SourceStepWithFilter::applyFilters(std::move(added_filter_nodes));

    const ActionsDAG::Node * predicate = nullptr;
    if (filter_actions_dag)
        predicate = filter_actions_dag->getOutputs().at(0);

    auto max_replicas_to_use = static_cast<UInt64>(cluster->getShardsInfo().size());
    if (context->getSettingsRef()[Setting::max_parallel_replicas] > 1)
        max_replicas_to_use = std::min(max_replicas_to_use, context->getSettingsRef()[Setting::max_parallel_replicas].value);

    createExtension(predicate, max_replicas_to_use);
}

void ReadFromCluster::createExtension(const ActionsDAG::Node * predicate, size_t number_of_replicas)
{
    if (extension)
        return;

    extension = storage->getTaskIteratorExtension(
        predicate,
        filter_actions_dag ? filter_actions_dag.get() : query_info.filter_actions_dag.get(),
        context,
        number_of_replicas);
}

/// The code executes on initiator
void IStorageCluster::read(
    QueryPlan & query_plan,
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr context,
    QueryProcessingStage::Enum processed_stage,
    size_t /*max_block_size*/,
    size_t /*num_streams*/)
{
    storage_snapshot->check(column_names);

    updateBeforeRead(context);
    auto cluster = getCluster(context);

    /// Calculate the header. This is significant, because some columns could be thrown away in some cases like query with count(*)

    SharedHeader sample_block;
    ASTPtr query_to_send = query_info.query;

    if (context->getSettingsRef()[Setting::allow_experimental_analyzer])
    {
        sample_block = InterpreterSelectQueryAnalyzer::getSampleBlock(query_info.query, context, SelectQueryOptions(processed_stage));
    }
    else
    {
        auto interpreter = InterpreterSelectQuery(query_info.query, context, SelectQueryOptions(processed_stage).analyze());
        sample_block = interpreter.getSampleBlock();
        query_to_send = interpreter.getQueryInfo().query->clone();
    }

    updateQueryToSendIfNeeded(query_to_send, storage_snapshot, context);

    RestoreQualifiedNamesVisitor::Data data;
    data.distributed_table = DatabaseAndTableWithAlias(*getTableExpression(query_info.query->as<ASTSelectQuery &>(), 0));
    data.remote_table.database = context->getCurrentDatabase();
    data.remote_table.table = getName();
    RestoreQualifiedNamesVisitor(data).visit(query_to_send);
    AddDefaultDatabaseVisitor visitor(context, context->getCurrentDatabase(),
                                      /* only_replace_current_database_function_= */false,
                                      /* only_replace_in_join_= */true);
    visitor.visit(query_to_send);

    auto this_ptr = std::static_pointer_cast<IStorageCluster>(shared_from_this());

    auto reading = std::make_unique<ReadFromCluster>(
        column_names,
        query_info,
        storage_snapshot,
        context,
        sample_block,
        std::move(this_ptr),
        std::move(query_to_send),
        processed_stage,
        cluster,
        log);

    query_plan.addStep(std::move(reading));
}

void ReadFromCluster::initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &)
{
    const Scalars & scalars = context->hasQueryContext() ? context->getQueryContext()->getScalars() : Scalars{};
    const bool add_agg_info = processed_stage == QueryProcessingStage::WithMergeableState;

    Pipes pipes;
    auto new_context = updateSettings(context->getSettingsRef());
    const auto & current_settings = new_context->getSettingsRef();
    auto timeouts = ConnectionTimeouts::getTCPTimeoutsWithFailover(current_settings);

    size_t replica_index = 0;
    auto max_replicas_to_use = static_cast<UInt64>(cluster->getShardsInfo().size());
    if (current_settings[Setting::max_parallel_replicas] > 1)
        max_replicas_to_use = std::min(max_replicas_to_use, current_settings[Setting::max_parallel_replicas].value);

    createExtension(nullptr, max_replicas_to_use);

    for (const auto & shard_info : cluster->getShardsInfo())
    {
        if (pipes.size() >= max_replicas_to_use)
            break;

        /// We're taking all replicas as shards,
        /// so each shard will have only one address to connect to.
        auto try_results = shard_info.pool->getMany(
            timeouts,
            current_settings,
            PoolMode::GET_ONE,
            {},
            /*skip_unavailable_endpoints=*/true);

        if (try_results.empty())
            continue;

        IConnections::ReplicaInfo replica_info{.number_of_current_replica = replica_index++};

        auto remote_query_executor = std::make_shared<RemoteQueryExecutor>(
            std::vector<IConnectionPool::Entry>{try_results.front()},
            query_to_send->formatWithSecretsOneLine(),
            getOutputHeader(),
            new_context,
            /*throttler=*/nullptr,
            scalars,
            Tables(),
            processed_stage,
            nullptr,
            RemoteQueryExecutor::Extension{.task_iterator = extension->task_iterator, .replica_info = std::move(replica_info)});

        remote_query_executor->setLogger(log);
        Pipe pipe{std::make_shared<RemoteSource>(
            remote_query_executor,
            add_agg_info,
            current_settings[Setting::async_socket_for_remote],
            current_settings[Setting::async_query_sending_for_remote])};
        pipe.addSimpleTransform([&](const SharedHeader & header) { return std::make_shared<UnmarshallBlocksTransform>(header); });
        pipes.emplace_back(std::move(pipe));
    }

    auto pipe = Pipe::unitePipes(std::move(pipes));
    if (pipe.empty())
        pipe = Pipe(std::make_shared<NullSource>(getOutputHeader()));

    for (const auto & processor : pipe.getProcessors())
        processors.emplace_back(processor);

    pipeline.init(std::move(pipe));
}

QueryProcessingStage::Enum IStorageCluster::getQueryProcessingStage(
    ContextPtr context, QueryProcessingStage::Enum to_stage, const StorageSnapshotPtr &, SelectQueryInfo &) const
{
    /// Initiator executes query on remote node.
    if (context->getClientInfo().query_kind == ClientInfo::QueryKind::INITIAL_QUERY)
        if (to_stage >= QueryProcessingStage::Enum::WithMergeableState)
            return QueryProcessingStage::Enum::WithMergeableState;

    /// Follower just reads the data.
    return QueryProcessingStage::Enum::FetchColumns;
}

ContextPtr ReadFromCluster::updateSettings(const Settings & settings)
{
    Settings new_settings{settings};

    /// Cluster table functions should always skip unavailable shards.
    new_settings[Setting::skip_unavailable_shards] = true;

    auto new_context = Context::createCopy(context);
    new_context->setSettings(new_settings);
    return new_context;
}

ClusterPtr IStorageCluster::getCluster(ContextPtr context) const
{
    return context->getCluster(cluster_name)->getClusterWithReplicasAsShards(context->getSettingsRef());
}

}
