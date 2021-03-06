/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include "macroblock.hh"
#include "tokens.hh"
#include "bool_decoder.hh"
#include "safe_array.hh"
#include "decoder.hh"

using namespace std;

template <unsigned int length>
uint16_t TokenDecoder<length>::decode( BoolDecoder & data ) const
{
  uint16_t increment = 0;
  for ( uint8_t i = 0; i < length; i++ ) {
    increment = ( increment << 1 ) + data.get( bit_probabilities_.at( i ) );
  }
  return base_value_ + increment;
}

/* The unfolded token decoder is not pretty, but it is considerably faster
   than using a tree decoder */

template < BlockType initial_block_type, class PredictionMode >
void Block< initial_block_type,
            PredictionMode >::parse_tokens( BoolDecoder & data,
                                            const ProbabilityTables & probability_tables )
{
  bool last_was_zero = false;

  /* prediction context starts with number-not-zero count */
  char token_context = ( context().above.initialized() ? context().above.get()->has_nonzero() : 0 )
    + ( context().left.initialized() ? context().left.get()->has_nonzero() : 0 );

  for ( unsigned int index = (type_ == BlockType::Y_after_Y2) ? 1 : 0;
        index < 16;
        index++ ) {
    /* select the tree probabilities based on the prediction context */
    const ProbabilityArray< MAX_ENTROPY_TOKENS > & prob
      = probability_tables.coeff_probs.at( type_ ).at( coefficient_to_band.at( index ) ).at( token_context );

    /* decode the token */
    if ( not last_was_zero ) {
      if ( not data.get( prob.at( 0 ) ) ) {
        /* EOB */
        break;
      }
    }

    if ( not data.get( prob.at( 1 ) ) ) {
      last_was_zero = true;
      token_context = 0;
      continue;
    }

    last_was_zero = false;
    has_nonzero_ = true;

    int16_t value;

    if ( not data.get( prob.at( 2 ) ) ) {
      value = 1;
      token_context = 1;
    } else {
      token_context = 2;
      if ( not data.get( prob.at( 3 ) ) ) {
        if ( not data.get( prob.at( 4 ) ) ) {
          value = 2;
        } else {
          if ( not data.get( prob.at( 5 ) ) ) {
            value = 3;
          } else {
            value = 4;
          }
        }
      } else {
        if ( not data.get( prob.at( 6 ) ) ) {
          if ( not data.get( prob.at( 7 ) ) ) {
            value = 5 + data.get( 159 );
          } else {
            value = token_decoder_1.decode( data );
          }
        } else {
          if ( not data.get( prob.at( 8 ) ) ) {
            if ( not data.get( prob.at( 9 ) ) ) {
              value = token_decoder_2.decode( data );
            } else {
              value = token_decoder_3.decode( data );
            }
          } else {
            if ( not data.get( prob.at( 10 ) ) ) {
              value = token_decoder_4.decode( data );
            } else {
              value = token_decoder_5.decode( data );
            }
          }
        }
      }
    }

    /* decode sign bit if absolute value is nonzero */
    if ( data.get() ) {
      value = -value;
    }

    /* assign to block storage */
    coefficients_.at( zigzag.at( index ) ) = value;
  }
}
