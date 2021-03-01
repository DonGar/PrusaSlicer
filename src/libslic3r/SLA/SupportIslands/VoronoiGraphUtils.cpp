#include "VoronoiGraphUtils.hpp"

#include <cmath>
#include <libslic3r/VoronoiOffset.hpp>
#include "IStackFunction.hpp"
#include "EvaluateNeighbor.hpp"
#include "ParabolaUtils.hpp"

#include <libslic3r/VoronoiVisualUtils.hpp>

using namespace Slic3r::sla;

VoronoiGraph::Node *VoronoiGraphUtils::getNode(VoronoiGraph &         graph,
                                               const VD::vertex_type *vertex,
                                               const VD::edge_type *  edge,
                                               const Lines &          lines)
{
    std::map<const VD::vertex_type *, VoronoiGraph::Node> &data = graph.data;
    auto &mapItem = data.find(vertex);
    // return when exists
    if (mapItem != data.end()) return &mapItem->second;

    // is new vertex (first edge to this vertex)
    // calculate distance to islad border + fill item0
    const VD::cell_type *cell = edge->cell();
    // const VD::cell_type *  cell2     = edge.twin()->cell();
    const Line &line = lines[cell->source_index()];
    // const Line &           line1     = lines[cell2->source_index()];
    Point  point(vertex->x(), vertex->y());
    double distance = line.distance_to(point);

    auto &[iterator,
           success] = data.emplace(vertex,
                                   VoronoiGraph::Node(vertex, distance));
    assert(success);
    return &iterator->second;
}

Slic3r::Point VoronoiGraphUtils::retrieve_point(const Lines &        lines,
                                                const VD::cell_type &cell)
{
    assert(cell.source_category() ==
               boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT ||
           cell.source_category() ==
               boost::polygon::SOURCE_CATEGORY_SEGMENT_END_POINT);
    return (cell.source_category() ==
            boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ?
               lines[cell.source_index()].a :
               lines[cell.source_index()].b;
}

Slic3r::Point VoronoiGraphUtils::get_parabola_point(
    const VD::edge_type &parabola, const Slic3r::Lines &lines)
{
    using namespace boost::polygon;
    assert(parabola.is_curved());
    const VD::cell_type& cell = (parabola.cell()->contains_point())?
                                *parabola.cell() : *parabola.twin()->cell();
    assert(cell.contains_point());
    assert(cell.source_category() == SOURCE_CATEGORY_SEGMENT_START_POINT ||
           cell.source_category() == SOURCE_CATEGORY_SEGMENT_END_POINT);
    return (cell.source_category() == SOURCE_CATEGORY_SEGMENT_START_POINT) ?
           lines[cell.source_index()].a :
           lines[cell.source_index()].b;
}

Slic3r::Line VoronoiGraphUtils::get_parabola_line(
    const VD::edge_type &parabola, const Slic3r::Lines &lines)
{
    assert(parabola.is_curved());
    const VD::cell_type& cell = (parabola.cell()->contains_segment())?
                                *parabola.cell() : *parabola.twin()->cell();
    assert(cell.contains_segment());
    return lines[cell.source_index()];
}

Parabola VoronoiGraphUtils::get_parabola(
    const VD::edge_type &edge, const Lines &lines)
{
    Point    point = get_parabola_point(edge, lines);
    Line     line  = get_parabola_line(edge, lines);
    return Parabola(line, point);
}

double VoronoiGraphUtils::calculate_length_of_parabola(
    const VD::edge_type &                               edge,
    const Lines &                                       lines)
{
    Point v0{edge.vertex0()->x(), edge.vertex0()->y()};
    Point v1{edge.vertex1()->x(), edge.vertex1()->y()};
    Parabola parabola = get_parabola(edge, lines);
    return ParabolaUtils::calculate_length_of_parabola(parabola, v0, v1);
}

double VoronoiGraphUtils::calculate_length(
    const VD::edge_type &edge, const Lines &lines)
{
    if (edge.is_linear()) {
        const VD::vertex_type* v0 = edge.vertex0();
        const VD::vertex_type* v1 = edge.vertex1();
        double diffX = v0->x() - v1->x();
        double diffY = v0->y() - v1->y();
        return sqrt(diffX * diffX + diffY * diffY);
    }    
    assert(edge.is_curved());
    return calculate_length_of_parabola(edge, lines);
}

double VoronoiGraphUtils::calculate_max_width(
    const VD::edge_type &edge, const Lines &lines)
{
    Point v0{edge.vertex0()->x(), edge.vertex0()->y()};
    Point v1{edge.vertex1()->x(), edge.vertex1()->y()};

    if (edge.is_linear()) {
        // line is initialized by 2 line segments only
        assert(!edge.cell()->contains_point());
        assert(edge.cell()->contains_segment());
        assert(!edge.twin()->cell()->contains_point());
        assert(edge.twin()->cell()->contains_segment());

        const Line &line = lines[edge.cell()->source_index()];

        double distance0 = line.perp_distance_to(v0);
        double distance1 = line.perp_distance_to(v1);
        return 2 * std::max(distance0, distance1);
    }
    assert(edge.is_curved());
    Parabola parabola = get_parabola(edge, lines);
    // distance to point and line is same
    // vector from edge vertex to parabola focus point
    Point  vec0 = parabola.focus - v0;
    Point  vec1 = parabola.focus - v1;
    double distance0 = sqrt(vec0.x() * vec0.x() + vec0.y() * vec0.y());
    double distance1 = sqrt(vec0.x() * vec0.x() + vec0.y() * vec0.y());
    return 2 * std::max(distance0, distance1);
}

VoronoiGraph VoronoiGraphUtils::getSkeleton(const VD &vd, const Lines &lines)
{
    // vd should be annotated.
    // assert(Voronoi::debug::verify_inside_outside_annotations(vd));

    VoronoiGraph         skeleton;
    const VD::edge_type *first_edge = &vd.edges().front();
    for (const VD::edge_type &edge : vd.edges()) {
        size_t edge_idx = &edge - first_edge;
        if (
            // Ignore secondary and unbounded edges, they shall never be part
            // of the skeleton.
            edge.is_secondary() || edge.is_infinite() ||
            // Skip the twin edge of an edge, that has already been processed.
            &edge > edge.twin() ||
            // Ignore outer edges.
            (Voronoi::edge_category(edge) !=
                 Voronoi::EdgeCategory::PointsInside &&
             Voronoi::edge_category(edge.twin()) !=
                 Voronoi::EdgeCategory::PointsInside))
            continue;

        const VD::vertex_type * v0        = edge.vertex0();
        const VD::vertex_type * v1        = edge.vertex1();
        Voronoi::VertexCategory category0 = Voronoi::vertex_category(*v0);
        Voronoi::VertexCategory category1 = Voronoi::vertex_category(*v1);
        if (category0 == Voronoi::VertexCategory::Outside ||
            category1 == Voronoi::VertexCategory::Outside)
            continue;
        // only debug check annotation
        if (category0 == Voronoi::VertexCategory::Unknown ||
            category1 == Voronoi::VertexCategory::Unknown)
            return {}; // vd must be annotated

        double length = calculate_length(edge, lines);
        double max_width = calculate_max_width(edge, lines);
        VoronoiGraph::Node *node0 = getNode(skeleton, v0, &edge, lines);
        VoronoiGraph::Node *node1 = getNode(skeleton, v1, &edge, lines);

        // add extended Edge to graph, both side
        VoronoiGraph::Node::Neighbor neighbor0(&edge, node1, length);
        node0->neighbors.push_back(neighbor0);
        VoronoiGraph::Node::Neighbor neighbor1(edge.twin(), node0, length);
        node1->neighbors.push_back(neighbor1);
    }
    return skeleton;
}

Slic3r::Point VoronoiGraphUtils::get_offseted_point(
    const VoronoiGraph::Node &node, double padding)
{
    assert(node.neighbors.size() == 1);
    const VoronoiGraph::Node::Neighbor &neighbor = node.neighbors.front();
    const VD::edge_type &               edge     = *neighbor.edge;
    const VD::vertex_type &             v0       = *edge.vertex0();
    const VD::vertex_type &             v1       = *edge.vertex1();
    Point                               dir(v0.x() - v1.x(), v0.y() - v1.y());
    if (node.vertex == &v0)
        dir *= -1;
    else
        assert(node.vertex == &v1);

    double size = neighbor.edge_length / padding;
    Point  move(dir[0] / size, dir[1] / size);
    return Point(node.vertex->x() + move[0], node.vertex->y() + move[1]);
}

const VoronoiGraph::Node::Neighbor *VoronoiGraphUtils::get_neighbor(
    const VoronoiGraph::Node *from, const VoronoiGraph::Node *to)
{
    for (const VoronoiGraph::Node::Neighbor &neighbor : from->neighbors)
        if (neighbor.node == to) return &neighbor;
    return nullptr;
}

double VoronoiGraphUtils::get_neighbor_distance(const VoronoiGraph::Node *from,
                                                const VoronoiGraph::Node *to)
{
    const VoronoiGraph::Node::Neighbor *neighbor = get_neighbor(from, to);
    assert(neighbor != nullptr);
    return neighbor->edge_length;
}

VoronoiGraph::Path VoronoiGraphUtils::find_longest_path_on_circle(
    const VoronoiGraph::Circle &                 circle,
    const VoronoiGraph::ExPath::SideBranchesMap &side_branches)
{
    double half_circle_length = circle.length / 2.;
    double distance_on_circle = 0;

    bool                      is_longest_revers_direction = false;
    const VoronoiGraph::Node *longest_circle_node         = nullptr;
    const VoronoiGraph::Path *longest_circle_branch       = nullptr;
    double                    longest_branch_length       = 0;

    bool is_short_revers_direction = false;
    // find longest side branch
    const VoronoiGraph::Node *prev_circle_node = nullptr;
    for (const VoronoiGraph::Node *circle_node : circle.nodes) {
        if (prev_circle_node != nullptr)
            distance_on_circle += get_neighbor_distance(circle_node,
                                                        prev_circle_node);
        prev_circle_node = circle_node;

        auto side_branches_item = side_branches.find(circle_node);
        if (side_branches_item != side_branches.end()) {
            // side_branches should be sorted by length
            if (distance_on_circle > half_circle_length)
                is_short_revers_direction = true;
            const auto &longest_node_branch = side_branches_item->second.top();
            double      circle_branch_length = longest_node_branch.length +
                                          ((is_short_revers_direction) ?
                                               (circle.length -
                                                distance_on_circle) :
                                               distance_on_circle);
            if (longest_branch_length < circle_branch_length) {
                longest_branch_length       = circle_branch_length;
                is_longest_revers_direction = is_short_revers_direction;
                longest_circle_node         = circle_node;
                longest_circle_branch       = &longest_node_branch;
            }
        }
    }
    assert(longest_circle_node !=
           nullptr); // only circle with no side branches
    assert(longest_circle_branch != nullptr);
    // almost same - double preccission
    // distance_on_circle += get_neighbor_distance(circle.path.back(),
    // circle.path.front()); assert(distance_on_circle == circle.length);

    // circlePath
    auto circle_iterator = std::find(circle.nodes.begin(), circle.nodes.end(),
                                     longest_circle_node);
    VoronoiGraph::Nodes circle_path;
    if (is_longest_revers_direction) {
        circle_path = VoronoiGraph::Nodes(circle_iterator, circle.nodes.end());
        std::reverse(circle_path.begin(), circle_path.end());
    } else {
        if (longest_circle_node != circle.nodes.front())
            circle_path = VoronoiGraph::Nodes(circle.nodes.begin() + 1,
                                              circle_iterator + 1);
    }
    // append longest side branch
    circle_path.insert(circle_path.end(),
                       longest_circle_branch->nodes.begin(),
                       longest_circle_branch->nodes.end());
    return {circle_path, longest_branch_length};
}

VoronoiGraph::Path VoronoiGraphUtils::find_longest_path_on_circles(
    const VoronoiGraph::Node &  input_node,
    size_t                      finished_circle_index,
    const VoronoiGraph::ExPath &ex_path)
{
    const std::vector<VoronoiGraph::Circle> &circles = ex_path.circles;
    const auto &circle                = circles[finished_circle_index];
    auto        connected_circle_item = ex_path.connected_circle.find(
        finished_circle_index);
    // is only one circle
    if (connected_circle_item == ex_path.connected_circle.end()) {
        // find longest path over circle and store it into next_path
        return find_longest_path_on_circle(circle, ex_path.side_branches);
    }

    // multi circle
    // find longest path over circles
    const std::set<size_t> &connected_circles = connected_circle_item->second;

    // collect all circle ndoes
    std::set<const VoronoiGraph::Node *> nodes;
    nodes.insert(circle.nodes.begin(), circle.nodes.end());
    for (size_t circle_index : connected_circles) {
        const auto &circle = circles[circle_index];
        nodes.insert(circle.nodes.begin(), circle.nodes.end());
    }

    // nodes are path throw circles
    // length is sum path throw circles PLUS length of longest side_branch
    VoronoiGraph::Path longest_path;

    // wide search by shortest distance for path over circle's node
    // !! Do NOT use recursion, may cause stack overflow
    std::set<const VoronoiGraph::Node *> done; // all ready checked
    // on top is shortest path
    std::priority_queue<VoronoiGraph::Path, std::vector<VoronoiGraph::Path>,
                        VoronoiGraph::Path::OrderLengthFromShortest>
                       search_queue;
    VoronoiGraph::Path start_path({&input_node}, 0.);
    search_queue.emplace(start_path);
    while (!search_queue.empty()) {
        // shortest path from input_node
        VoronoiGraph::Path path(std::move(search_queue.top()));
        search_queue.pop();
        const VoronoiGraph::Node &node = *path.nodes.back();
        if (done.find(&node) != done.end()) { // already checked
            continue;
        }
        done.insert(&node);
        for (const VoronoiGraph::Node::Neighbor &neighbor : node.neighbors) {
            if (nodes.find(neighbor.node) == nodes.end())
                continue; // out of circles
            if (done.find(neighbor.node) != done.end()) continue;
            VoronoiGraph::Path neighbor_path = path; // make copy
            neighbor_path.append(neighbor.node, neighbor.edge_length);
            search_queue.push(neighbor_path);

            auto branches_item = ex_path.side_branches.find(neighbor.node);
            // exist side from this neighbor node ?
            if (branches_item == ex_path.side_branches.end()) continue;
            const VoronoiGraph::Path &longest_branch = branches_item->second
                                                           .top();
            double length = longest_branch.length + neighbor_path.length;
            if (longest_path.length < length) {
                longest_path.length = length;
                longest_path.nodes  = neighbor_path.nodes; // copy path
            }
        }
    }

    // create result path
    assert(!longest_path.nodes.empty());
    longest_path.nodes.erase(longest_path.nodes.begin()); // remove input_node
    assert(!longest_path.nodes.empty());
    auto branches_item = ex_path.side_branches.find(longest_path.nodes.back());
    if (branches_item == ex_path.side_branches.end()) {
        // longest path ends on circle
        return longest_path;
    }
    const VoronoiGraph::Path &longest_branch = branches_item->second.top();
    longest_path.nodes.insert(longest_path.nodes.end(),
                              longest_branch.nodes.begin(),
                              longest_branch.nodes.end());
    return longest_path;
}

std::optional<VoronoiGraph::Circle> VoronoiGraphUtils::create_circle(
    const VoronoiGraph::Path &          path,
    const VoronoiGraph::Node::Neighbor &neighbor)
{
    VoronoiGraph::Nodes passed_nodes = path.nodes;
    // detection of circle
    // not neccesary to check last one in path
    auto        end_find  = passed_nodes.end() - 1;
    const auto &path_item = std::find(passed_nodes.begin(), end_find,
                                      neighbor.node);
    if (path_item == end_find) return {}; // circle not detected
    // separate Circle:
    VoronoiGraph::Nodes circle_path(path_item, passed_nodes.end());
    // !!! Real circle lenght is calculated on detection of end circle
    // now circle_length contain also lenght of path before circle
    double circle_length = path.length + neighbor.edge_length;
    // solve of branch length will be at begin of cirlce
    return VoronoiGraph::Circle(std::move(circle_path), circle_length);
};

void VoronoiGraphUtils::merge_connected_circle(
    VoronoiGraph::ExPath::ConnectedCircles &dst,
    VoronoiGraph::ExPath::ConnectedCircles &src,
    size_t                                  dst_circle_count)
{
    std::set<size_t> done;
    for (const auto &item : src) {
        size_t dst_index = dst_circle_count + item.first;
        if (done.find(dst_index) != done.end()) continue;
        done.insert(dst_index);

        std::set<size_t> connected_circle;
        for (const size_t &src_index : item.second)
            connected_circle.insert(dst_circle_count + src_index);

        auto &dst_set = dst[dst_index];
        dst_set.merge(connected_circle);

        // write same information into connected circles
        connected_circle = dst_set; // copy
        connected_circle.insert(dst_index);
        for (size_t prev_connection_idx : dst_set) {
            done.insert(prev_connection_idx);
            for (size_t connected_circle_idx : connected_circle) {
                if (connected_circle_idx == prev_connection_idx) continue;
                dst[prev_connection_idx].insert(connected_circle_idx);
            }
        }
    }
}

void VoronoiGraphUtils::append_neighbor_branch(VoronoiGraph::ExPath &dst,
                                               VoronoiGraph::ExPath &src)
{
    // move side branches
    if (!src.side_branches.empty())
        dst.side_branches
            .insert(std::make_move_iterator(src.side_branches.begin()),
                    std::make_move_iterator(src.side_branches.end()));

    // move circles
    if (!src.circles.empty()) {
        // copy connected circles indexes
        if (!src.connected_circle.empty()) {
            merge_connected_circle(dst.connected_circle, src.connected_circle,
                                   dst.circles.size());
        }
        dst.circles.insert(dst.circles.end(),
                           std::make_move_iterator(src.circles.begin()),
                           std::make_move_iterator(src.circles.end()));
    }
}

void VoronoiGraphUtils::reshape_longest_path(VoronoiGraph::ExPath &path)
{
    assert(path.nodes.size() >= 1);

    double                    actual_length = 0.;
    const VoronoiGraph::Node *prev_node     = nullptr;
    VoronoiGraph::Nodes       origin_path   = path.nodes; // make copy
    // index to path
    size_t path_index = 0;
    for (const VoronoiGraph::Node *node : origin_path) {
        if (prev_node != nullptr) {
            ++path_index;
            actual_length += get_neighbor_distance(prev_node, node);
        }
        prev_node = node;
        // increase actual length

        auto side_branches_item = path.side_branches.find(node);
        if (side_branches_item == path.side_branches.end())
            continue; // no side branches
        VoronoiGraph::ExPath::SideBranches &branches = side_branches_item
                                                           ->second;
        if (actual_length >= branches.top().length)
            continue; // no longer branch

        auto               end_path = path.nodes.begin() + path_index;
        VoronoiGraph::Path side_branch({path.nodes.begin(), end_path},
                                       actual_length);
        std::reverse(side_branch.nodes.begin(), side_branch.nodes.end());
        VoronoiGraph::Path new_main_branch(std::move(branches.top()));
        branches.pop();
        std::reverse(new_main_branch.nodes.begin(),
                     new_main_branch.nodes.end());
        // add old main path store into side branches - may be it is not neccessary
        branches.push(std::move(side_branch));

        // swap side branch with main branch
        path.nodes.erase(path.nodes.begin(), end_path);
        path.nodes.insert(path.nodes.begin(), new_main_branch.nodes.begin(),
                          new_main_branch.nodes.end());

        path.length += new_main_branch.length;
        path.length -= actual_length;
        path_index    = new_main_branch.nodes.size();
        actual_length = new_main_branch.length;
    }
}

VoronoiGraph::ExPath VoronoiGraphUtils::create_longest_path(
    const VoronoiGraph::Node *start_node)
{
    VoronoiGraph::ExPath longest_path;
    CallStack            call_stack;
    call_stack.emplace(
        std::make_unique<EvaluateNeighbor>(longest_path, start_node));

    // depth search for longest path in graph
    while (!call_stack.empty()) {
        std::unique_ptr<IStackFunction> stack_function = std::move(
            call_stack.top());
        call_stack.pop();
        stack_function->process(call_stack);
        // stack function deleted
    }

    reshape_longest_path(longest_path);
    // after reshape it shoud be longest path for whole Voronoi Graph
    return longest_path;
}

Slic3r::Point VoronoiGraphUtils::get_edge_point(const VD::edge_type *edge, double ratio)
{
    const VD::vertex_type *v0 = edge->vertex0();
    const VD::vertex_type *v1 = edge->vertex1();
    if (ratio <= std::numeric_limits<double>::epsilon())
        return Point(v0->x(), v0->y());
    if (ratio >= 1. - std::numeric_limits<double>::epsilon())
        return Point(v1->x(), v1->y());

    if (edge->is_linear()) {
        Point dir(
            v1->x() - v0->x(), 
            v1->y() - v0->y());
        // normalize
        dir *= ratio;
        return Point(v0->x() + dir.x(), v0->y() + dir.y());
    }

    assert(edge->is_curved());
    // TODO: distance on curve

    // approx by line
    Point dir(v1->x() - v0->x(), v1->y() - v0->y());
    dir *= ratio;
    return Point(v0->x() + dir.x(), v0->y() + dir.y());
}

Slic3r::Point VoronoiGraphUtils::get_point_on_path(const VoronoiGraph::Nodes &path, double distance)
{
    const VoronoiGraph::Node *prev_node        = nullptr;
    double                    actual_distance         = 0.;
    for (const VoronoiGraph::Node *node : path) {
        if (prev_node == nullptr) { // first call
            prev_node = node;
            continue;
        }
        const VoronoiGraph::Node::Neighbor *neighbor = get_neighbor(prev_node, node);
        actual_distance += neighbor->edge_length;
        if (actual_distance >= distance) {
            // over half point is on
            double previous_distance = actual_distance - distance;
            double over_ratio = previous_distance / neighbor->edge_length;
            double ratio = 1. - over_ratio;
            return get_edge_point(neighbor->edge, ratio);
        }
        prev_node = node;
    }
    // distance must be inside path
    // this means bad input params
    assert(false);
    return Point(0, 0);
}

std::vector<Slic3r::Point> VoronoiGraphUtils::sample_longest_path(
    const VoronoiGraph::ExPath &longest_path, const SampleConfig &config)
{
    // 1) One support point
    if (longest_path.length <
        config.max_length_for_one_support_point) { // create only one
                                                   // point in center
        // sample in center of voronoi
        return {get_center_of_path(longest_path.nodes, longest_path.length)};
    }
    // 2) Two support points
    //if (longest_path.length < config.max_distance) {}

    std::vector<Point> points;
    points.push_back(get_offseted_point(*longest_path.nodes.front(), config.start_distance));

    return points;
}

std::vector<Slic3r::Point> VoronoiGraphUtils::sample_voronoi_graph(
    const VoronoiGraph &  graph,
    const SampleConfig &  config,
    VoronoiGraph::ExPath &longest_path)
{
    // first vertex on contour:
    const VoronoiGraph::Node *start_node = nullptr;
    for (const auto &[key, value] : graph.data) {
        const VD::vertex_type & vertex   = *key;
        Voronoi::VertexCategory category = Voronoi::vertex_category(vertex);
        if (category == Voronoi::VertexCategory::OnContour) {
            start_node = &value;
            break;
        }
    }
    // every island has to have a point on contour
    assert(start_node != nullptr);
    longest_path = create_longest_path(start_node);
    // longest_path = create_longest_path_recursive(start_node);
    return sample_longest_path(longest_path, config);
}

void VoronoiGraphUtils::draw(SVG &               svg,
                             const VoronoiGraph &graph,
                             coord_t             width)
{
    for (const auto &[key, value] : graph.data) {
        svg.draw(Point(key->x(), key->y()), "lightgray", width);
        for (const auto &n : value.neighbors) {
            if (n.edge->vertex0() > n.edge->vertex1()) continue;
            auto  v0 = *n.edge->vertex0();
            Point from(v0.x(), v0.y());
            auto  v1 = *n.edge->vertex1();
            Point to(v1.x(), v1.y());
            svg.draw(Line(from, to), "gray", width);

            Point center = from + to;
            center *= .5;
            // svg.draw_text(center,
            // (std::to_string(std::round(n.edge_length/3e5)/100.)).c_str(), "gray");
        }
    }
}

void VoronoiGraphUtils::draw(SVG &                      svg,
                             const VoronoiGraph::Nodes &path,
                             coord_t                    width,
                             const char *               color,
                             bool                       finish)
{
    const VoronoiGraph::Node *prev_node = (finish) ? path.back() : nullptr;
    int                       index     = 0;
    for (auto &node : path) {
        ++index;
        if (prev_node == nullptr) {
            prev_node = node;
            continue;
        }
        Point from(prev_node->vertex->x(), prev_node->vertex->y());
        Point to(node->vertex->x(), node->vertex->y());
        svg.draw(Line(from, to), color, width);

        svg.draw_text(from, std::to_string(index - 1).c_str(), color);
        svg.draw_text(to, std::to_string(index).c_str(), color);
        prev_node = node;
    }
}

void VoronoiGraphUtils::draw(SVG &                       svg,
                             const VoronoiGraph::ExPath &path,
                             coord_t                     width)
{
    const char *circlePathColor   = "green";
    const char *sideBranchesColor = "blue";
    const char *mainPathColor     = "red";

    for (auto &circle : path.circles) {
        draw(svg, circle.nodes, width, circlePathColor, true);
        Point center(0, 0);
        for (auto p : circle.nodes) {
            center.x() += p->vertex->x();
            center.y() += p->vertex->y();
        }
        center.x() /= circle.nodes.size();
        center.y() /= circle.nodes.size();

        svg.draw_text(center,
                      ("C" + std::to_string(&circle - &path.circles.front()))
                          .c_str(),
                      circlePathColor);
    }

    for (const auto &branches : path.side_branches) {
        auto tmp = branches.second; // copy
        while (!tmp.empty()) {
            const auto &branch = tmp.top();
            auto        path   = branch.nodes;
            path.insert(path.begin(), branches.first);
            draw(svg, path, width, sideBranchesColor);
            tmp.pop();
        }
    }

    draw(svg, path.nodes, width, mainPathColor);
}

