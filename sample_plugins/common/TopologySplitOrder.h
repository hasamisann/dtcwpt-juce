#pragma once

#include <string>

namespace topology
{
struct SplitChild
{
    int nodeId {};
    std::string path;
};

struct SplitChildren
{
    SplitChild highChild;
    SplitChild lowChild;
};

inline SplitChildren makeHighFirstSplitChildren (int parentNodeId, const std::string& parentPath)
{
    SplitChildren children;
    children.highChild = { (2 * parentNodeId) + 1, parentPath + "H" };
    children.lowChild  = { 2 * parentNodeId, parentPath + "L" };
    return children;
}
} // namespace topology
