/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file linkgraph.cpp Definition of link graph classes used for cargo distribution. */

#include "../stdafx.h"
#include "../core/pool_func.hpp"
#include "linkgraph.h"

#include "../safeguards.h"

/* Initialize the link-graph-pool */
LinkGraphPool _link_graph_pool("LinkGraph");
INSTANTIATE_POOL_METHODS(LinkGraph)

/**
 * Create a node or clear it.
 * @param xy Location of the associated station.
 * @param st ID of the associated station.
 * @param demand Demand for cargo at the station.
 */
LinkGraph::BaseNode::BaseNode(TileIndex xy, StationID st, uint demand)
{
	this->xy = xy;
	this->supply = 0;
	this->demand = demand;
	this->station = st;
	this->last_update = INVALID_DATE;
}

/**
 * Create an edge.
 */
LinkGraph::BaseEdge::BaseEdge(NodeID dest_node)
{
	this->capacity = 0;
	this->usage = 0;
	this->travel_time_sum = 0;
	this->last_unrestricted_update = INVALID_DATE;
	this->last_restricted_update = INVALID_DATE;
	this->dest_node = dest_node;
}

/**
 * Shift all dates by given interval.
 * This is useful if the date has been modified with the cheat menu.
 * @param interval Number of days to be added or subtracted.
 */
void LinkGraph::ShiftDates(int interval)
{
	this->last_compression += interval;
	for (NodeID node1 = 0; node1 < this->Size(); ++node1) {
		BaseNode &source = this->nodes[node1];
		if (source.last_update != INVALID_DATE) source.last_update += interval;
		for (BaseEdge &edge : this->nodes[node1].edges) {
			if (edge.last_unrestricted_update != INVALID_DATE) edge.last_unrestricted_update += interval;
			if (edge.last_restricted_update != INVALID_DATE) edge.last_restricted_update += interval;
		}
	}
}

void LinkGraph::Compress()
{
	this->last_compression = (_date + this->last_compression) / 2;
	for (NodeID node1 = 0; node1 < this->Size(); ++node1) {
		this->nodes[node1].supply /= 2;
		for (BaseEdge &edge : this->nodes[node1].edges) {
			if (edge.capacity > 0) {
				uint new_capacity = std::max(1U, edge.capacity / 2);
				if (edge.capacity < (1 << 16)) {
					edge.travel_time_sum = edge.travel_time_sum * new_capacity / edge.capacity;
				} else if (edge.travel_time_sum != 0) {
					edge.travel_time_sum = std::max(1ULL, edge.travel_time_sum / 2);
				}
				edge.capacity = new_capacity;
				edge.usage /= 2;
			}
		}
	}
}

/**
 * Merge a link graph with another one.
 * @param other LinkGraph to be merged into this one.
 */
void LinkGraph::Merge(LinkGraph *other)
{
	Date age = _date - this->last_compression + 1;
	Date other_age = _date - other->last_compression + 1;
	NodeID first = this->Size();
	for (NodeID node1 = 0; node1 < other->Size(); ++node1) {
		Station *st = Station::Get(other->nodes[node1].station);
		NodeID new_node = this->AddNode(st);
		this->nodes[new_node].supply = LinkGraph::Scale(other->nodes[node1].supply, age, other_age);
		st->goods[this->cargo].link_graph = this->index;
		st->goods[this->cargo].node = new_node;

		for (BaseEdge &e : other->nodes[node1].edges) {
			BaseEdge &new_edge = this->nodes[new_node].edges.emplace_back(first + e.dest_node);
			new_edge.capacity = LinkGraph::Scale(e.capacity, age, other_age);
			new_edge.usage = LinkGraph::Scale(e.usage, age, other_age);
			new_edge.travel_time_sum = LinkGraph::Scale(e.travel_time_sum, age, other_age);
		}
	}
	delete other;
}

/**
 * Remove a node from the link graph by overwriting it with the last node.
 * @param id ID of the node to be removed.
 */
void LinkGraph::RemoveNode(NodeID id)
{
	assert(id < this->Size());

	NodeID last_node = this->Size() - 1;
	Station::Get(this->nodes[last_node].station)->goods[this->cargo].node = id;
	/* Erase node by swapping with the last element. Node index is referenced
	 * directly from station goods entries so the order and position must remain. */
	this->nodes[id] = this->nodes.back();
	this->nodes.pop_back();
	for (auto &n : this->nodes) {
		/* Find iterator position where an edge to id would be. */
		auto [first, last] = std::equal_range(n.edges.begin(), n.edges.end(), id);
		/* Remove potential node (erasing an empty range is safe). */
		auto insert = n.edges.erase(first, last);
		/* As the edge list is sorted, a potential edge to last_node will always be the last edge. */
		if (!n.edges.empty() && n.edges.back().dest_node == last_node) {
			/* Change dest ID and move into the spot of the deleted edge. */
			n.edges.back().dest_node = id;
			n.edges.insert(insert, n.edges.back());
			n.edges.pop_back();
		}
	}
}

/**
 * Add a node to the component and create empty edges associated with it. Set
 * the station's last_component to this component. Calculate the distances to all
 * other nodes. The distances to _all_ nodes are important as the demand
 * calculator relies on their availability.
 * @param st New node's station.
 * @return New node's ID.
 */
NodeID LinkGraph::AddNode(const Station *st)
{
	const GoodsEntry &good = st->goods[this->cargo];

	NodeID new_node = this->Size();
	this->nodes.emplace_back(st->xy, st->index, HasBit(good.status, GoodsEntry::GES_ACCEPTANCE));

	return new_node;
}

/**
 * Fill an edge with values from a link. Set the restricted or unrestricted
 * update timestamp according to the given update mode.
 * @param to Destination node of the link.
 * @param capacity Capacity of the link.
 * @param usage Usage to be added.
 * @param mode Update mode to be used.
 */
void LinkGraph::Node::AddEdge(NodeID to, uint capacity, uint usage, uint32 travel_time, EdgeUpdateMode mode)
{
	assert(this->index != to);
	assert(!this->HasEdgeTo(to));

	BaseEdge &edge = *this->node.edges.emplace(std::upper_bound(this->node.edges.begin(), this->node.edges.end(), to), to);
	edge.capacity = capacity;
	edge.usage = usage;
	edge.travel_time_sum = static_cast<uint64>(travel_time) * capacity;
	if (mode & EUM_UNRESTRICTED)  edge.last_unrestricted_update = _date;
	if (mode & EUM_RESTRICTED) edge.last_restricted_update = _date;
}

/**
 * Creates an edge if none exists yet or updates an existing edge.
 * @param to Target node.
 * @param capacity Capacity of the link.
 * @param usage Usage to be added.
 * @param mode Update mode to be used.
 */
void LinkGraph::Node::UpdateEdge(NodeID to, uint capacity, uint usage, uint32 travel_time, EdgeUpdateMode mode)
{
	assert(capacity > 0);
	assert(usage <= capacity);
	if (!this->HasEdgeTo(to)) {
		this->AddEdge(to, capacity, usage, travel_time, mode);
	} else {
		(*this)[to].Update(capacity, usage, travel_time, mode);
	}
}

/**
 * Remove an outgoing edge from this node.
 * @param to ID of destination node.
 */
void LinkGraph::Node::RemoveEdge(NodeID to)
{
	auto [first, last] = std::equal_range(this->node.edges.begin(), this->node.edges.end(), to);
	this->node.edges.erase(first, last);
}

/**
 * Update an edge. If mode contains UM_REFRESH refresh the edge to have at
 * least the given capacity and usage, otherwise add the capacity, usage and travel time.
 * In any case set the respective update timestamp(s), according to the given
 * mode.
 * @param capacity Capacity to be added/updated.
 * @param usage Usage to be added.
 * @param travel_time Travel time to be added, in ticks.
 * @param mode Update mode to be applied.
 */
void LinkGraph::Edge::Update(uint capacity, uint usage, uint32 travel_time, EdgeUpdateMode mode)
{
	assert(this->edge.capacity > 0);
	assert(capacity >= usage);

	if (mode & EUM_INCREASE) {
		if (this->edge.travel_time_sum == 0) {
			this->edge.travel_time_sum = static_cast<uint64>(this->edge.capacity + capacity) * travel_time;
		} else if (travel_time == 0) {
			this->edge.travel_time_sum += this->edge.travel_time_sum / this->edge.capacity * capacity;
		} else {
			this->edge.travel_time_sum += static_cast<uint64>(travel_time) * capacity;
		}
		this->edge.capacity += capacity;
		this->edge.usage += usage;
	} else if (mode & EUM_REFRESH) {
		if (this->edge.travel_time_sum == 0) {
			this->edge.capacity = std::max(this->edge.capacity, capacity);
			this->edge.travel_time_sum = static_cast<uint64>(travel_time) * this->edge.capacity;
		} else if (capacity > this->edge.capacity) {
			this->edge.travel_time_sum = this->edge.travel_time_sum / this->edge.capacity * capacity;
			this->edge.capacity = capacity;
		}
		this->edge.usage = std::max(this->edge.usage, usage);
	}
	if (mode & EUM_UNRESTRICTED) this->edge.last_unrestricted_update = _date;
	if (mode & EUM_RESTRICTED) this->edge.last_restricted_update = _date;
}

/**
 * Resize the component and fill it with empty nodes and edges. Used when
 * loading from save games. The component is expected to be empty before.
 * @param size New size of the component.
 */
void LinkGraph::Init(uint size)
{
	assert(this->Size() == 0);
	this->nodes.resize(size);
}
