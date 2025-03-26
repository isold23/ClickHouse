#pragma once

#include <Storages/StorageView.h>
#include <Processors/QueryPlan/ITransformingStep.h>

namespace DB
{

class CustomMetricLogStep : public ITransformingStep
{
public:
    CustomMetricLogStep(Block input_header_, Block output_header_);

    String getName() const override
    {
        return "CustomMetricLogStep";
    }

    void transformPipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &) override;

    void updateOutputHeader() override {}
};


class StorageSystemMetricLogView final : public IStorage
{
public:
    explicit StorageSystemMetricLogView(const StorageID & table_id_);

    std::string getName() const override { return "SystemMetricLogView"; }

    bool isSystemStorage() const override { return true; }

    void checkAlterIsPossible(const AlterCommands & commands, ContextPtr local_context) const override;

    void read(
        QueryPlan & query_plan,
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        size_t num_streams) override;

private:
    StorageView internal_view;

};

}
