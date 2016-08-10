/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <limits>

#include "encoder.hh"
#include "scorer.hh"

using namespace std;

static bool out_of_bounds( const MotionVector & mv )
{
  int16_t x = mv.x();
  int16_t y = mv.y();

  if ( x >  1023 ) return true;
  if ( x < -1023 ) return true;
  if ( y >  1023 ) return true;
  if ( y < -1023 ) return true;

  return false;
}

/*
 * Taken from: libvpx:vp8/encoder/rdopt.c:135
 */
static const array<int, 128> sad_per_bit16lut = {
  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  3,  3,  3,
  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,
  4,  4,  4,  4,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  6,  6,  6,
  6,  6,  6,  6,  6,  6,  6,  6,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
  7,  7,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9,
  9,  9,  9,  9,  9,  9,  9,  10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11,
  11, 11, 12, 12, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14
};

MotionVector Encoder::diamond_search( const VP8Raster::Macroblock & original_mb,
                                      VP8Raster::Macroblock & reconstructed_mb,
                                      VP8Raster::Macroblock & temp_mb,
                                      InterFrameMacroblock & frame_mb,
                                      const VP8Raster & reference,
                                      MotionVector base_mv,
                                      MotionVector origin,
                                      size_t step_size ) const
{
  TwoDSubRange<uint8_t, 16, 16> & prediction = temp_mb.Y.mutable_contents();

  base_mv = Scorer::clamp( base_mv, frame_mb.context() );

  while ( step_size > 0 ) {
    MBPredictionData best_pred;
    MBPredictionData pred;

    for ( int x = -1; x <= 1; x++ ) {
      for ( int y = -1; y <= 1; y++ ) {
        if ( ( ( x & y ) == 0 ) && ( ( x | y ) != 0 ) ) continue;

        pred.mv = MotionVector();
        MotionVector direction( ( ( step_size << 1 ) * ( x + y ) ),
                                ( ( step_size << 1 ) * ( x - y ) ) );

        pred.mv += origin + direction;

        if ( out_of_bounds( pred.mv ) ) continue;

        MotionVector this_mv( Scorer::clamp( pred.mv + base_mv, frame_mb.context() ) );

        reconstructed_mb.Y.inter_predict( this_mv, reference.Y(), prediction );
        pred.distortion = sad( original_mb.Y, prediction );
        pred.rate = costs_.sad_motion_vector_cost( pred.mv, MotionVector(), sad_per_bit16lut[ qindex_ ] );
        pred.cost = rdcost( pred.rate, pred.distortion, 1, 1 );

        if ( pred.cost < best_pred.cost  ) {
          best_pred = pred;
        }
      }
    }

    origin = best_pred.mv;
    step_size /= 2;
  }

  return origin;
}

void Encoder::luma_mb_inter_predict( const VP8Raster::Macroblock & original_mb,
                                     VP8Raster::Macroblock & reconstructed_mb,
                                     VP8Raster::Macroblock & temp_mb,
                                     InterFrameMacroblock & frame_mb,
                                     const Quantizer & quantizer,
                                     const EncoderPass encoder_pass )
{
  // let's find the best intra-prediction for this macroblock first...
  MBPredictionData best_pred = luma_mb_best_prediction_mode( original_mb,
                                                             reconstructed_mb,
                                                             temp_mb,
                                                             frame_mb,
                                                             quantizer,
                                                             encoder_pass,
                                                             true );

  MotionVector best_mv;
  const VP8Raster & reference = references_.last.get();
  const auto & reference_mb = reference.macroblock( frame_mb.context().column,
                                                    frame_mb.context().row );

  TwoDSubRange<uint8_t, 16, 16> & prediction = temp_mb.Y.mutable_contents();

  const Scorer census = frame_mb.motion_vector_census();
  const auto counts = census.mode_contexts();
  const ProbabilityArray< num_mv_refs > mv_ref_probs = {{ mv_counts_to_probs.at( counts.at( 0 ) ).at( 0 ),
                                                          mv_counts_to_probs.at( counts.at( 1 ) ).at( 1 ),
                                                          mv_counts_to_probs.at( counts.at( 2 ) ).at( 2 ),
                                                          mv_counts_to_probs.at( counts.at( 3 ) ).at( 3 ) }};

  costs_.fill_mv_ref_costs( mv_ref_probs );
  costs_.fill_mv_component_costs( decoder_state_.probability_tables.motion_vector_probs );
  costs_.fill_mv_sad_costs();

  vector<mbmode> inter_modes = { ZEROMV, NEARESTMV, NEARMV, NEWMV, /* SPLIMV */ };

  for ( const mbmode prediction_mode : inter_modes ) {
    MBPredictionData pred;
    pred.prediction_mode = prediction_mode;
    MotionVector mv;

    switch ( prediction_mode ) {
    case NEWMV:
      for ( size_t step = 7; step < 8; step-- ) {
        mv = diamond_search( original_mb, reconstructed_mb, temp_mb, frame_mb,
                             reference, census.best(), mv, ( 1 << step ) );
      }

      mv = Scorer::clamp( mv, frame_mb.context() );

      if ( mv.empty() ) {
        continue;
      }

      break;

    case NEARESTMV:
    case NEARMV:
      mv = Scorer::clamp( ( prediction_mode == NEARMV ) ? census.near() : census.nearest(),
                          frame_mb.context() );

      if ( mv.empty() ) {
        // Same as ZEROMV
        continue;
      }

      break;

    case ZEROMV:
      mv = MotionVector();
      break;

    default:
      throw runtime_error( "not supported" );
    }

    reference_mb.Y.inter_predict( mv, reference.Y(), prediction );
    pred.distortion = variance( original_mb.Y, prediction );
    pred.rate = costs_.mbmode_costs.at( 1 ).at( prediction_mode );

    if ( prediction_mode == NEWMV ) {
      pred.rate += costs_.motion_vector_cost( mv );
    }

    pred.cost = rdcost( pred.rate, pred.distortion, RATE_MULTIPLIER,
                        DISTORTION_MULTIPLIER );

    if ( pred.cost < best_pred.cost ) {
      best_mv = mv;
      best_pred = pred;
      reconstructed_mb.Y.mutable_contents().copy_from( prediction );
    }
  }

  if ( best_pred.prediction_mode <= B_PRED ) {
    // This block will be intra-predicted
    frame_mb.mutable_header().is_inter_mb = false;

    luma_mb_apply_intra_prediction( original_mb, reconstructed_mb, temp_mb,
                                    frame_mb, quantizer,
                                    best_pred.prediction_mode, encoder_pass );
  }
  else {
    // This block will be inter-predicted
    frame_mb.mutable_header().is_inter_mb = true;
    frame_mb.mutable_header().set_reference( LAST_FRAME );

    frame_mb.Y2().set_prediction_mode( best_pred.prediction_mode );
    frame_mb.set_base_motion_vector( best_mv );

    frame_mb.Y().forall(
      [&] ( YBlock & frame_sb ) { frame_sb.set_motion_vector( frame_mb.base_motion_vector() ); }
    );

    SafeArray<int16_t, 16> walsh_input;

    // XXX refactor this and its keyframe counterpart to remove the redundancy.
    frame_mb.Y().forall_ij(
      [&] ( YBlock & frame_sb, unsigned int sb_column, unsigned int sb_row )
      {
        auto & original_sb = original_mb.Y_sub.at( sb_column, sb_row );

        frame_sb.mutable_coefficients().subtract_dct( original_sb,
          reconstructed_mb.Y_sub.at( sb_column, sb_row ).contents() );

        walsh_input.at( sb_column + 4 * sb_row ) = frame_sb.coefficients().at( 0 );
        frame_sb.set_dc_coefficient( 0 );
        frame_sb.set_Y_after_Y2();

        frame_sb.mutable_coefficients() = YBlock::quantize( quantizer, frame_sb.coefficients() );
        frame_sb.calculate_has_nonzero();
      }
    );

    frame_mb.Y2().set_coded( true );
    frame_mb.Y2().mutable_coefficients().wht( walsh_input );
    frame_mb.Y2().mutable_coefficients() = Y2Block::quantize( quantizer, frame_mb.Y2().coefficients() );
    frame_mb.Y2().calculate_has_nonzero();
  }
}

void Encoder::chroma_mb_inter_predict( const VP8Raster::Macroblock & original_mb,
                                       VP8Raster::Macroblock & reconstructed_mb,
                                       VP8Raster::Macroblock & temp_mb,
                                       InterFrameMacroblock & frame_mb,
                                       const Quantizer & quantizer,
                                       const EncoderPass ) const
{
  assert( frame_mb.inter_coded() );

  const VP8Raster & reference = references_.last.get();
  const auto & reference_mb = reference.macroblock( frame_mb.context().column,
                                                    frame_mb.context().row );

  TwoDSubRange<uint8_t, 8, 8> & u_prediction = temp_mb.U.mutable_contents();
  TwoDSubRange<uint8_t, 8, 8> & v_prediction = temp_mb.V.mutable_contents();

  frame_mb.U().forall_ij(
    [&]( UVBlock & block, const unsigned int column, const unsigned int row )
    {
      block.set_motion_vector( MotionVector::luma_to_chroma(
        frame_mb.Y().at( column * 2,     row * 2     ).motion_vector(),
        frame_mb.Y().at( column * 2 + 1, row * 2     ).motion_vector(),
        frame_mb.Y().at( column * 2,     row * 2 + 1 ).motion_vector(),
        frame_mb.Y().at( column * 2 + 1, row * 2 + 1 ).motion_vector() ) );
    }
  );

  reference_mb.U.inter_predict( frame_mb.U().at( 0, 0 ).motion_vector(),
                                reference.U(), u_prediction );
  reference_mb.V.inter_predict( frame_mb.U().at( 0, 0 ).motion_vector(),
                                reference.V(), v_prediction );

  reconstructed_mb.U.mutable_contents().copy_from( u_prediction );
  reconstructed_mb.V.mutable_contents().copy_from( v_prediction );

  frame_mb.U().forall_ij(
    [&] ( UVBlock & frame_sb, unsigned int sb_column, unsigned int sb_row )
    {
      auto & original_sb = original_mb.U_sub.at( sb_column, sb_row );

      frame_sb.mutable_coefficients().subtract_dct( original_sb,
        reconstructed_mb.U_sub.at( sb_column, sb_row ).contents() );

      frame_sb.mutable_coefficients() = UVBlock::quantize( quantizer, frame_sb.coefficients() );
      frame_sb.calculate_has_nonzero();
    }
  );

  frame_mb.V().forall_ij(
    [&] ( UVBlock & frame_sb, unsigned int sb_column, unsigned int sb_row )
    {
      auto & original_sb = original_mb.V_sub.at( sb_column, sb_row );

      frame_sb.mutable_coefficients().subtract_dct( original_sb,
        reconstructed_mb.V_sub.at( sb_column, sb_row ).contents() );

      frame_sb.mutable_coefficients() = UVBlock::quantize( quantizer, frame_sb.coefficients() );
      frame_sb.calculate_has_nonzero();
    }
  );
}

void Encoder::optimize_interframe_probs( InterFrame & frame )
{
  size_t inter_count  = 0;
  size_t last_count   = 0;
  size_t golden_count = 0;
  size_t total_count  = 0;

  frame.mutable_macroblocks().forall(
    [&] ( InterFrameMacroblock & frame_mb )
    {
      total_count++;

      switch( frame_mb.header().reference() ) {
      case CURRENT_FRAME:
        inter_count++;
        break;

      case LAST_FRAME:
        last_count++;
        break;

      case GOLDEN_FRAME:
        golden_count++;
        break;

      case ALTREF_FRAME:
        break;
      }
    }
  );

  frame.mutable_header().prob_inter = Encoder::calc_prob( inter_count, total_count );
  frame.mutable_header().prob_references_last = Encoder::calc_prob( last_count, total_count );
  frame.mutable_header().prob_references_golden = Encoder::calc_prob( golden_count, total_count );
}

template<>
pair<InterFrame, double> Encoder::encode_with_quantizer<InterFrame>( const VP8Raster & raster,
                                                                     const QuantIndices & quant_indices,
                                                                     const bool update_state )
{
  DecoderState decoder_state_copy = decoder_state_;

  InterFrame frame = Encoder::make_empty_frame<InterFrame>( width_, height_ );
  frame.mutable_header().quant_indices = quant_indices;
  frame.mutable_header().refresh_entropy_probs = true;
  frame.mutable_header().refresh_last = true;

  Quantizer quantizer( frame.header().quant_indices );
  MutableRasterHandle reconstructed_raster_handle { width_, height_ };
  VP8Raster & reconstructed_raster = reconstructed_raster_handle.get();

  TokenBranchCounts token_branch_counts;

  raster.macroblocks().forall_ij(
    [&] ( VP8Raster::Macroblock & original_mb, unsigned int mb_column, unsigned int mb_row )
    {
      auto & reconstructed_mb = reconstructed_raster.macroblock( mb_column, mb_row );
      auto & temp_mb = temp_raster().macroblock( mb_column, mb_row );
      auto & frame_mb = frame.mutable_macroblocks().at( mb_column, mb_row );

      // Process Y and Y2
      luma_mb_inter_predict( original_mb, reconstructed_mb, temp_mb, frame_mb,
                             quantizer, FIRST_PASS );

      if ( frame_mb.inter_coded() ) {
        chroma_mb_inter_predict( original_mb, reconstructed_mb, temp_mb,
                                 frame_mb, quantizer, FIRST_PASS );
      }
      else {
        chroma_mb_intra_predict( original_mb, reconstructed_mb, temp_mb,
                                 frame_mb, quantizer, FIRST_PASS );
      }

      frame_mb.calculate_has_nonzero();

      if ( frame_mb.inter_coded() ) {
        frame_mb.reconstruct_inter( quantizer, references_, reconstructed_mb );
      }
      else {
        frame_mb.reconstruct_intra( quantizer, reconstructed_mb );
      }

      frame_mb.accumulate_token_branches( token_branch_counts );
    }
  );

  frame.relink_y2_blocks();

  optimize_prob_skip( frame );
  optimize_interframe_probs( frame );
  optimize_probability_tables( frame, token_branch_counts );
  apply_best_loopfilter_settings( raster, reconstructed_raster, frame );

  if ( not update_state ) {
    decoder_state_ = decoder_state_copy;
  }
  else {
    references_.last = move( reconstructed_raster_handle );
    reference_flags_.has_last = true;
  }

  return make_pair( move( frame ), reconstructed_raster.quality( raster ) );
}