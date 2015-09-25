#include "tracking_player.hh"
#include "frame.hh"

using namespace std;

template<class FrameType>
SerializedFrame TrackingPlayer::decode_and_serialize( const FrameType & frame, const Decoder & source,
                                                      const Chunk & compressed_frame )
{
  pair<bool, RasterHandle> output = decoder_.decode_frame( frame );

  SourceHash source_hash = source.source_hash( frame.get_used() );
  TargetHash target_hash = decoder_.target_hash( frame.get_updated(),
                                                 output.second, output.first );

  // Check this is a sane frame
  assert( source.get_hash().can_decode( source_hash ) );
  assert( decoder_.get_hash().continuation_hash() == target_hash.continuation_hash );

  return SerializedFrame( compressed_frame, source_hash, target_hash, make_optional( output.first, output.second ) );
}

SerializedFrame TrackingPlayer::serialize_next( void )
{
  Chunk compressed_frame = get_next_frame();

  // Save the source so we can hash the parts of it that are used by
  // the next frame;
  Decoder source = decoder_;

  UncompressedChunk decompressed_frame = decoder_.decompress_frame( compressed_frame );
  if ( decompressed_frame.key_frame() ) {
    KeyFrame frame = decoder_.parse_frame<KeyFrame>( decompressed_frame );

    return decode_and_serialize( frame, source, compressed_frame );
  } else {
    InterFrame frame = decoder_.parse_frame<InterFrame>( decompressed_frame );

    return decode_and_serialize( frame, source, compressed_frame );
  }
}

RasterHandle TrackingPlayer::next_output( void )
{
  UncompressedChunk decompressed_frame = decoder_.decompress_frame( get_next_frame() );

  if ( decompressed_frame.key_frame() ) {
    return decoder_.decode_frame( decoder_.parse_frame<KeyFrame>( decompressed_frame ) ).second;
  } else {
    return decoder_.decode_frame( decoder_.parse_frame<InterFrame>( decompressed_frame ) ).second;
  }
}
