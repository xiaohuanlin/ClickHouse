#pragma once

#include <mutex>

#include <Core/Block.h>
#include <Interpreters/IJoin.h>
#include <Interpreters/TableJoin.h>


namespace DB
{

/// Used when setting 'join_algorithm' set to JoinAlgorithm::AUTO.
/// Starts JOIN with join-in-memory algorithm and switches to join-on-disk on the fly if there's no memory to place right table.
/// Current join-in-memory and join-on-disk are JoinAlgorithm::HASH and JoinAlgorithm::PARTIAL_MERGE joins respectively.
class JoinSwitcher : public IJoin
{
public:
    JoinSwitcher(std::shared_ptr<TableJoin> table_join_, SharedHeader right_sample_block_);

    std::string getName() const override { return "JoinSwitcher"; }
    const TableJoin & getTableJoin() const override { return *table_join; }

    /// Add block of data from right hand of JOIN into current join object.
    /// If join-in-memory memory limit exceeded switches to join-on-disk and continue with it.
    /// @returns false, if join-on-disk disk limit exceeded
    bool addBlockToJoin(const Block & block, bool check_limits) override;

    void checkTypesOfKeys(const Block & block) const override
    {
        join->checkTypesOfKeys(block);
    }

    JoinResultPtr joinBlock(Block block) override
    {
        return join->joinBlock(block);
    }

    const Block & getTotals() const override
    {
        return join->getTotals();
    }

    void setTotals(const Block & block) override
    {
        join->setTotals(block);
    }

    size_t getTotalRowCount() const override
    {
        return join->getTotalRowCount();
    }

    size_t getTotalByteCount() const override
    {
        return join->getTotalByteCount();
    }

    bool alwaysReturnsEmptySet() const override
    {
        return join->alwaysReturnsEmptySet();
    }

    IBlocksStreamPtr
    getNonJoinedBlocks(const Block & left_sample_block, const Block & result_sample_block, UInt64 max_block_size) const override
    {
        return join->getNonJoinedBlocks(left_sample_block, result_sample_block, max_block_size);
    }

    IBlocksStreamPtr getDelayedBlocks() override
    {
        return join->getDelayedBlocks();
    }

    bool hasDelayedBlocks() const override
    {
        return join->hasDelayedBlocks();
    }

    void onBuildPhaseFinish() override { join->onBuildPhaseFinish(); }

private:
    JoinPtr join;
    SizeLimits limits;
    bool switched;
    mutable std::mutex switch_mutex;
    std::shared_ptr<TableJoin> table_join;
    const Block right_sample_block;

    /// Change join-in-memory to join-on-disk moving right hand JOIN data from one to another.
    /// Throws an error if join-on-disk do not support JOIN kind or strictness.
    bool switchJoin();
};

}
