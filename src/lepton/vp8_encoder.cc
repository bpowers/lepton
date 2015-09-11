/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <string>
#include <cassert>
#include <iostream>
#include <fstream>

#include "bitops.hh"
#include "component_info.hh"
#include "uncompressed_components.hh"
#include "jpgcoder.hh"
#include "vp8_encoder.hh"

#include "block.hh"
#include "bool_encoder.hh"
#include "model.hh"
#include "numeric.hh"
#include "slice.hh"
#include "../io/SwitchableCompression.hh"
#include "../vp8/model/model.hh"
#include "../vp8/encoder/encoder.hh"

using namespace std;
void printContext(FILE * fp) {
    for (int cm= 0;cm< 3;++cm) {
        for (int y = 0;y < Context::H/8; ++y) {
            for (int x = 0;x < Context::W/8; ++x) {
                for (int by = 0; by < 8; ++by){
                    for (int bx = 0; bx < 8; ++bx) {
                        for (int ctx = 0;ctx < NUMCONTEXT;++ctx) {
                            for (int dim = 0; dim < 3; ++dim) {
#ifdef ANNOTATION_ENABLED
                                int val = 0;
                                val = gctx->p[cm][y][x][by][bx][ctx][dim];
                                const char *nam = "UNKNOWN";
                                switch (ctx) {
                                  case ZDSTSCAN:nam = "ZDSTSCAN";break;
                                  case ZEROS7x7:nam = "ZEROS7x7";break;
                                  case EXPDC:nam = "EXPDC";break;
                                  case RESDC:nam = "RESDC";break;
                                  case SIGNDC:nam = "SIGNDC";break;
                                  case EXP7x7:nam = "EXP7x7";break;
                                  case RES7x7:nam = "RES7x7";break;
                                  case SIGN7x7:nam = "SIGN7x7";break;
                                  case ZEROS1x8:nam = "ZEROS1x8";break;
                                  case ZEROS8x1:nam = "ZEROS8x1";break;
                                  case EXP8:nam = "EXP8";break;
                                  case THRESH8: nam = "THRESH8"; break;
                                  case RES8:nam = "RES8";break;
                                  case SIGN8:nam = "SIGN8";break;
                                  default:break;
                                }
                                if (val != -1 && ctx != ZDSTSCAN) {
                                    fprintf(fp, "col[%02d] y[%02d]x[%02d] by[%02d]x[%02d] [%s][%d] = %d\n",
                                            cm, y, x, by, bx, nam, dim, val);
                                }
#endif
                            }
                        }
                    }
                }
            }
        }
    }
}

CodingReturnValue VP8ComponentEncoder::encode_chunk(const UncompressedComponents *input,
                                                    Sirikata::
                                                    SwitchableCompressionWriter<Sirikata::
                                                                                DecoderCompressionWriter> *output) {
    return vp8_full_encoder(input, output);
}

template<class Left, class Middle, class Right>
void VP8ComponentEncoder::process_row(Left & left_model,
                                      Middle& middle_model,
                                      Right& right_model,
                                      int block_width,
                                      const UncompressedComponents * const colldata,
                                      Sirikata::Array1d<KVContext,
                                              (uint32_t)ColorChannel::NumBlockTypes> &context,
                                      BoolEncoder &bool_encoder) {
    if (block_width > 0) {
        ConstBlockContext block_context = context.at((int)Middle::COLOR).context;
        const AlignedBlock &block = block_context.here();
#ifdef ANNOTATION_ENABLED
        gctx->cur_cmp = component; // for debug purposes only, not to be used in production
        gctx->cur_jpeg_x = 0;
        gctx->cur_jpeg_y = curr_y;
#endif
        block.recalculate_coded_length();
        serialize_tokens(block_context,
                         bool_encoder,
                         left_model);
        context.at((int)Middle::COLOR).context = colldata->full_component_nosync(Middle::COLOR).next(block_context);
    }
    for ( int jpeg_x = 1; jpeg_x + 1 < block_width; jpeg_x++ ) {
        ConstBlockContext block_context = context.at((int)Middle::COLOR).context;
        const AlignedBlock &block = block_context.here();
#ifdef ANNOTATION_ENABLED
        gctx->cur_cmp = component; // for debug purposes only, not to be used in production
        gctx->cur_jpeg_x = jpeg_x;
        gctx->cur_jpeg_y = curr_y;
#endif
        block.recalculate_coded_length();
        serialize_tokens(block_context,
                         bool_encoder,
                         middle_model);
        context.at((int)Middle::COLOR).context = colldata->full_component_nosync(Middle::COLOR).next(block_context);
    }
    if (block_width > 1) {
        ConstBlockContext block_context = context.at((int)Middle::COLOR).context;
        const AlignedBlock &block = block_context.here();
#ifdef ANNOTATION_ENABLED
        gctx->cur_cmp = Middle::COLOR; // for debug purposes only, not to be used in production
        gctx->cur_jpeg_x = block_width - 1;
        gctx->cur_jpeg_y = curr_y;
#endif
        block.recalculate_coded_length();
        serialize_tokens(block_context,
                         bool_encoder,
                         right_model);
        context.at((int)Middle::COLOR).context = colldata->full_component_nosync(Middle::COLOR).next(block_context);
    }
}

CodingReturnValue VP8ComponentEncoder::vp8_full_encoder( const UncompressedComponents * const colldata,
                                            Sirikata::
                                            SwitchableCompressionWriter<Sirikata::
                                                                        DecoderCompressionWriter> *str_out)
{
    /* cmpc is a global variable with the component count */
    using namespace Sirikata;
    Array1d<KVContext, (uint32_t)ColorChannel::NumBlockTypes> context;
    for (size_t i = 0; i < context.size(); ++i) {
        context[i].context = colldata->full_component_nosync(i).begin();
        context[i].y = 0;
    }
    str_out->EnableCompression();

    /* read in probability table coeff probs */
    ProbabilityTablesBase::load_probability_tables();

    /* get ready to serialize the blocks */
    BoolEncoder bool_encoder;
    BlockType component = BlockType::Y;
    ProbabilityTablesBase::set_quantization_table(BlockType::Y, colldata->get_quantization_tables(BlockType::Y));
    ProbabilityTablesBase::set_quantization_table(BlockType::Cb, colldata->get_quantization_tables(BlockType::Cb));
    ProbabilityTablesBase::set_quantization_table(BlockType::Cr, colldata->get_quantization_tables(BlockType::Cr));
    tuple<ProbabilityTables<false, false, false, BlockType::Y>,
          ProbabilityTables<false, false, false, BlockType::Cb>,
          ProbabilityTables<false, false, false, BlockType::Cr> > corner;

    tuple<ProbabilityTables<true, false, false, BlockType::Y>,
          ProbabilityTables<true, false, false, BlockType::Cb>,
          ProbabilityTables<true, false, false, BlockType::Cr> > top;

    tuple<ProbabilityTables<false, true, true, BlockType::Y>,
          ProbabilityTables<false, true, true, BlockType::Cb>,
          ProbabilityTables<false, true, true, BlockType::Cr> > midleft;

    tuple<ProbabilityTables<true, true, true, BlockType::Y>,
          ProbabilityTables<true, true, true, BlockType::Cb>,
          ProbabilityTables<true, true, true, BlockType::Cr> > middle;

    tuple<ProbabilityTables<true, true, false, BlockType::Y>,
          ProbabilityTables<true, true, false, BlockType::Cb>,
          ProbabilityTables<true, true, false, BlockType::Cr> > midright;

    tuple<ProbabilityTables<false, true, false, BlockType::Y>,
          ProbabilityTables<false, true, false, BlockType::Cb>,
          ProbabilityTables<false, true, false, BlockType::Cr> > width_one;
    while(colldata->get_next_component(context, &component)) {
        int curr_y = context.at((int)component).y;
        int block_width = colldata->block_width( component );
        if (curr_y == 0) {
            switch(component) {
                case BlockType::Y:
                    process_row(std::get<(int)BlockType::Y>(corner),
                                std::get<(int)BlockType::Y>(top),
                                std::get<(int)BlockType::Y>(top),
                                block_width,
                                colldata,
                                context,
                                bool_encoder);
                    break;
                case BlockType::Cb:
                    process_row(std::get<(int)BlockType::Cb>(corner),
                                std::get<(int)BlockType::Cb>(top),
                                std::get<(int)BlockType::Cb>(top),
                                block_width,
                                colldata,
                                context,
                                bool_encoder);
                    break;
                case BlockType::Cr:
                    process_row(std::get<(int)BlockType::Cr>(corner),
                                std::get<(int)BlockType::Cr>(top),
                                std::get<(int)BlockType::Cr>(top),
                                block_width,
                                colldata,
                                context,
                                bool_encoder);
                    break;
            }
        } else if (block_width > 1) {
            switch(component) {
                case BlockType::Y:
                    process_row(std::get<(int)BlockType::Y>(midleft),
                                std::get<(int)BlockType::Y>(middle),
                                std::get<(int)BlockType::Y>(midright),
                                block_width,
                                colldata,
                                context,
                                bool_encoder);
                    break;
                case BlockType::Cb:
                    process_row(std::get<(int)BlockType::Cb>(midleft),
                                std::get<(int)BlockType::Cb>(middle),
                                std::get<(int)BlockType::Cb>(midright),
                                block_width,
                                colldata,
                                context,
                                bool_encoder);
                    break;
                case BlockType::Cr:
                    process_row(std::get<(int)BlockType::Cr>(midleft),
                                std::get<(int)BlockType::Cr>(middle),
                                std::get<(int)BlockType::Cr>(midright),
                                block_width,
                                colldata,
                                context,
                                bool_encoder);
                    break;
            }
        } else {
            assert(block_width == 1);
            switch(component) {
                case BlockType::Y:
                    process_row(std::get<(int)BlockType::Y>(width_one),
                                std::get<(int)BlockType::Y>(width_one),
                                std::get<(int)BlockType::Y>(width_one),
                                block_width,
                                colldata,
                                context,
                                bool_encoder);
                    break;
                case BlockType::Cb:
                    process_row(std::get<(int)BlockType::Cb>(width_one),
                                std::get<(int)BlockType::Cb>(width_one),
                                std::get<(int)BlockType::Cb>(width_one),
                                block_width,
                                colldata,
                                context,
                                bool_encoder);
                    break;
                case BlockType::Cr:
                    process_row(std::get<(int)BlockType::Cr>(width_one),
                                std::get<(int)BlockType::Cr>(width_one),
                                std::get<(int)BlockType::Cr>(width_one),
                                block_width,
                                colldata,
                                context,
                                bool_encoder);
                    break;
            }
        }

        ++context.at((int)component).y;
    }

    /* get coded output */
    const auto stream = bool_encoder.finish();

    /* write block header */
    str_out->Write( reinterpret_cast<const unsigned char*>("x"), 1 );
    str_out->DisableCompression();

    /* write length */
    const uint32_t length_big_endian =
        htobe32( stream.size() );
    str_out->Write( reinterpret_cast<const unsigned char*>(&length_big_endian), sizeof( uint32_t ));

    /* write coded octet stream */
    str_out->Write( &stream.at( 0 ), stream.size() );

    /* possibly write out new probability model */
    const char * out_model_name = getenv( "LEPTON_COMPRESSION_MODEL_OUT" );
    if ( out_model_name ) {
        cerr << "Writing new compression model..." << endl;

        std::ofstream model_file { out_model_name };
        if ( not model_file.good() ) {
            std::cerr << "error writing to " + string( out_model_name ) << std::endl;
            return CODING_ERROR;
        }

        std::get<(int)BlockType::Y>(middle).optimize();
        std::get<(int)BlockType::Y>(middle).serialize( model_file );
    }
#ifdef ANNOTATION_ENABLED
    {
        FILE * fp = fopen("/tmp/lepton.ctx","w");
        printContext(fp);
        fclose(fp);
    }
#endif
    return CODING_DONE;
}
