/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2014 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#ifndef MAPNIK_RENDERER_COMMON_PROCESS_GROUP_SYMBOLIZER_HPP
#define MAPNIK_RENDERER_COMMON_PROCESS_GROUP_SYMBOLIZER_HPP

// mapnik
#include <mapnik/pixel_position.hpp>
#include <mapnik/marker_cache.hpp>
#include <mapnik/feature.hpp>
#include <mapnik/feature_factory.hpp>
#include <mapnik/renderer_common.hpp>
#include <mapnik/symbolizer.hpp>
#include <mapnik/attribute_collector.hpp>
#include <mapnik/group/group_layout_manager.hpp>
#include <mapnik/group/group_symbolizer_helper.hpp>
#include <mapnik/group/group_symbolizer_properties.hpp>
#include <mapnik/text/glyph_positions.hpp>
#include <mapnik/util/conversions.hpp>
#include <mapnik/util/variant.hpp>
#include <mapnik/label_collision_detector.hpp>
#include <mapnik/noncopyable.hpp>

// agg
#include <agg_trans_affine.h>

namespace mapnik {

class proj_transform;
struct glyph_info;
class text_symbolizer_helper;

struct virtual_renderer_common : private mapnik::noncopyable
{

    virtual_renderer_common(renderer_common & common) :
        width_(common.width_),
        height_(common.height_),
        scale_factor_(common.scale_factor_),
        vars_(common.vars_),
        shared_font_library_(common.shared_font_library_),
        font_library_(*shared_font_library_),
        font_manager_(common.font_manager_),
        query_extent_(common.query_extent_),
        t_(common.t_),
        detector_(std::make_shared<label_collision_detector4>(common.detector_->extent())) {}

    unsigned & width_;
    unsigned & height_;
    double & scale_factor_;
    attributes & vars_;
    // TODO: dirty hack for cairo renderer, figure out how to remove this
    std::shared_ptr<font_library> & shared_font_library_;
    font_library & font_library_;
    face_manager_freetype & font_manager_;
    box2d<double> & query_extent_;
    view_transform & t_;
    std::shared_ptr<label_collision_detector4> detector_;
};


// General:

// The approach here is to run the normal symbolizers, but in
// a 'virtual' blank environment where the changes that they
// make are recorded (the detector, the render_* calls).
//
// The recorded boxes are then used to lay out the items and
// the offsets from old to new positions can be used to perform
// the actual rendering calls.

// This should allow us to re-use as much as possible of the
// existing symbolizer layout and rendering code while still
// being able to interpose our own decisions about whether
// a collision has occured or not.

// Thunk for rendering a particular instance of a point - this
// stores all the arguments necessary to re-render this point
// symbolizer at a later time.

struct point_render_thunk : noncopyable
{
    pixel_position pos_;
    marker_ptr marker_;
    agg::trans_affine tr_;
    double opacity_;
    composite_mode_e comp_op_;

    point_render_thunk(pixel_position const& pos, marker const& m,
                       agg::trans_affine const& tr, double opacity,
                       composite_mode_e comp_op);
    point_render_thunk(point_render_thunk && rhs)
      : pos_(std::move(rhs.pos_)),
        marker_(std::move(rhs.marker_)),
        tr_(std::move(rhs.tr_)),
        opacity_(std::move(rhs.opacity_)),
        comp_op_(std::move(rhs.comp_op_)) {}
};

using helper_ptr = std::unique_ptr<text_symbolizer_helper>;

struct text_render_thunk : noncopyable
{
    // helper is stored here in order
    // to keep in scope the text rendering structures
    helper_ptr helper_;
    placements_list const& placements_;
    double opacity_;
    composite_mode_e comp_op_;
    halo_rasterizer_enum halo_rasterizer_;

    text_render_thunk(helper_ptr && helper,
                      double opacity, composite_mode_e comp_op,
                      halo_rasterizer_enum halo_rasterizer);
    text_render_thunk(text_render_thunk && rhs)
      : helper_(std::move(rhs.helper_)),
        placements_(std::move(rhs.placements_)),
        opacity_(std::move(rhs.opacity_)),
        comp_op_(std::move(rhs.comp_op_)),
        halo_rasterizer_(std::move(rhs.halo_rasterizer_)) {}

};

// Variant type for render thunks to allow us to re-render them
// via a static visitor later.

using render_thunk = util::variant<point_render_thunk,
                                   text_render_thunk>;
using render_thunk_ptr = std::unique_ptr<render_thunk>;
using render_thunk_list = std::list<render_thunk_ptr>;

// Base class for extracting the bounding boxes associated with placing
// a symbolizer at a fake, virtual point - not real geometry.
//
// The bounding boxes can be used for layout, and the thunks are
// used to re-render at locations according to the group layout.

struct render_thunk_extractor : public util::static_visitor<>
{
    render_thunk_extractor(box2d<double> & box,
                           render_thunk_list & thunks,
                           feature_impl & feature,
                           attributes const& vars,
                           proj_transform const& prj_trans,
                           virtual_renderer_common & common,
                           box2d<double> const& clipping_extent);

    void operator()(point_symbolizer const& sym) const;

    void operator()(text_symbolizer const& sym) const;

    void operator()(shield_symbolizer const& sym) const;

    template <typename T>
    void operator()(T const& ) const
    {
        // TODO: warning if unimplemented?
    }

private:
    void extract_text_thunk(helper_ptr && helper, text_symbolizer const& sym) const;

    box2d<double> & box_;
    render_thunk_list & thunks_;
    feature_impl & feature_;
    attributes const& vars_;
    proj_transform const& prj_trans_;
    virtual_renderer_common & common_;
    box2d<double> clipping_extent_;

    void update_box() const;
};

geometry_type *origin_point(proj_transform const& prj_trans,
                            renderer_common const& common);

template <typename F>
void render_offset_placements(placements_list const& placements,
                              pixel_position const& offset,
                              F render_text) {

    for (glyph_positions_ptr glyphs : placements)
    {
        // move the glyphs to the correct offset
        pixel_position base_point = glyphs->get_base_point();
        glyphs->set_base_point(base_point + offset);

        // update the position of any marker
        marker_info_ptr marker_info = glyphs->marker();
        pixel_position marker_pos = glyphs->marker_pos();
        if (marker_info)
        {
            glyphs->set_marker(marker_info, marker_pos + offset);
        }

        render_text(glyphs);

        // Need to put the base_point back how it was in case something else calls this again
        // (don't want to add offset twice) or calls with a different offset.
        glyphs->set_base_point(base_point);
        if (marker_info)
        {
            glyphs->set_marker(marker_info, marker_pos);
        }
    }
}

template <typename F>
void render_group_symbolizer(group_symbolizer const& sym,
                             feature_impl & feature,
                             attributes const& vars,
                             proj_transform const& prj_trans,
                             box2d<double> const& clipping_extent,
                             renderer_common & common,
                             F render_thunks)
{
    // find all column names referenced in the group rules and symbolizers
    std::set<std::string> columns;
    group_attribute_collector column_collector(columns, false);
    column_collector(sym);

    group_symbolizer_properties_ptr props = get<group_symbolizer_properties_ptr>(sym, keys::group_properties);

    // create a new context for the sub features of this group
    context_ptr sub_feature_ctx = std::make_shared<mapnik::context_type>();

    // populate new context with column names referenced in the group rules and symbolizers
    for (auto const& col_name : columns)
    {
        sub_feature_ctx->push(col_name);
    }

    // keep track of the sub features that we'll want to symbolize
    // along with the group rules that they matched
    std::vector< std::pair<group_rule_ptr, feature_ptr> > matches;

    // create a copied 'virtual' common renderer for processing sub feature symbolizers
    // create an empty detector for it, so we are sure we won't hit anything
    virtual_renderer_common virtual_renderer(common);

    // keep track of which lists of render thunks correspond to
    // entries in the group_layout_manager.
    std::vector<render_thunk_list> layout_thunks;
    size_t num_layout_thunks = 0;

    // layout manager to store and arrange bboxes of matched features
    group_layout_manager layout_manager(props->get_layout(), pixel_position(common.width_ / 2.0, common.height_ / 2.0));

    // run feature or sub feature through the group rules & symbolizers
    // for each index value in the range
    int start = get<value_integer>(sym, keys::start_column);
    int end = start + get<value_integer>(sym, keys::num_columns);
    for (int col_idx = start; col_idx < end; ++col_idx)
    {
        // create sub feature with indexed column values
        feature_ptr sub_feature = feature_factory::create(sub_feature_ctx, col_idx);

        // copy the necessary columns to sub feature
        for(auto const& col_name : columns)
        {
            if (col_name.find('%') != std::string::npos)
            {
                if (col_name.size() == 1)
                {
                    // column name is '%' by itself, so give the index as the value
                    sub_feature->put(col_name, (value_integer)col_idx);
                }
                else
                {
                    // indexed column
                    std::string col_idx_str;
                    if (mapnik::util::to_string(col_idx_str,col_idx))
                    {
                        std::string col_idx_name = col_name;
                        boost::replace_all(col_idx_name, "%", col_idx_str);
                        sub_feature->put(col_name, feature.get(col_idx_name));
                    }
                }
            }
            else
            {
                // non-indexed column
                sub_feature->put(col_name, feature.get(col_name));
            }
        }

        // add a single point geometry at pixel origin
        sub_feature->add_geometry(origin_point(prj_trans, common));

        // get the layout for this set of properties
        for (auto const& rule : props->get_rules())
        {
             if (util::apply_visitor(evaluate<Feature,value_type,attributes>(*sub_feature,common.vars_),
                                               *(rule->get_filter())).to_bool())
             {
                // add matched rule and feature to the list of things to draw
                matches.emplace_back(rule, sub_feature);

                // construct a bounding box around all symbolizers for the matched rule
                bound_box bounds;
                render_thunk_list thunks;
                render_thunk_extractor extractor(bounds, thunks, *sub_feature, common.vars_, prj_trans,
                                                 virtual_renderer, clipping_extent);

                for (auto const& sym : *rule)
                {
                    // TODO: construct layout and obtain bounding box
                    util::apply_visitor(extractor, sym);
                }

                // add the bounding box to the layout manager
                layout_manager.add_member_bound_box(bounds);
                layout_thunks.emplace_back(std::move(thunks));
                ++num_layout_thunks;
                break;
            }
        }
    }

    // create a symbolizer helper
    group_symbolizer_helper helper(sym, feature, vars, prj_trans,
                                   common.width_, common.height_,
                                   common.scale_factor_, common.t_,
                                   *common.detector_, clipping_extent);

    for (size_t i = 0; i < matches.size(); ++i)
    {
        group_rule_ptr match_rule = matches[i].first;
        feature_ptr match_feature = matches[i].second;
        value_unicode_string rpt_key_value = "";

        // get repeat key from matched group rule
        expression_ptr rpt_key_expr = match_rule->get_repeat_key();

        // if no repeat key was defined, use default from group symbolizer
        if (!rpt_key_expr)
        {
            rpt_key_expr = get<expression_ptr>(sym, keys::repeat_key);
        }

        // evalute the repeat key with the matched sub feature if we have one
        if (rpt_key_expr)
        {
            rpt_key_value = util::apply_visitor(evaluate<Feature,value_type,attributes>(*match_feature,common.vars_),
                                                *rpt_key_expr).to_unicode();
        }
        helper.add_box_element(layout_manager.offset_box_at(i), rpt_key_value);
    }

    pixel_position_list positions = helper.get();
    for (pixel_position const& pos : positions)
    {
        for (size_t layout_i = 0; layout_i < num_layout_thunks; ++layout_i)
        {
            pixel_position const& offset = layout_manager.offset_at(layout_i);
            pixel_position render_offset = pos + offset;
            render_thunks(layout_thunks[layout_i], render_offset);
        }
    }
}

} // namespace mapnik

#endif // MAPNIK_RENDERER_COMMON_PROCESS_GROUP_SYMBOLIZER_HPP
