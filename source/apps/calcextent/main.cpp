/*******************************************************************************
#      ____               __          __  _      _____ _       _               #
#     / __ \              \ \        / / | |    / ____| |     | |              #
#    | |  | |_ __   ___ _ __ \  /\  / /__| |__ | |  __| | ___ | |__   ___      #
#    | |  | | '_ \ / _ \ '_ \ \/  \/ / _ \ '_ \| | |_ | |/ _ \| '_ \ / _ \     #
#    | |__| | |_) |  __/ | | \  /\  /  __/ |_) | |__| | | (_) | |_) |  __/     #
#     \____/| .__/ \___|_| |_|\/  \/ \___|_.__/ \_____|_|\___/|_.__/ \___|     #
#           | |                                                                #
#           |_|                                                                #
#                                                                              #
#                                (c) 2011 by                                   #
#           University of Applied Sciences Northwestern Switzerland            #
#                     Institute of Geomatics Engineering                       #
#                           martin.christen@fhnw.ch                            #
********************************************************************************
*     Licensed under MIT License. Read the file LICENSE for more information   *
*******************************************************************************/

//------------------------------------------------------------------------------

#include "og.h"
#include "ogprocess.h"
#include "geo/MercatorQuadtree.h"
#include "geo/CoordinateTransformation.h"
#include "string/FilenameUtils.h"
#include "string/StringUtils.h"
#include "io/FileSystem.h"
#include <float.h>
#include <iostream>
#include <ctime>
#include <boost/program_options.hpp>


int _frominput(const std::vector<std::string>& vecFiles, const std::string& srs, bool bVerbose);
void _calcfromwgs84(int, double, double, double, double);
//------------------------------------------------------------------------------

namespace po = boost::program_options;

int main(int argc, char *argv[])
{
   po::options_description desc("Program-Options");
   desc.add_options()
       ("maxlod", po::value<int>(), "desired level of detail (integer)")
       ("wgs84", po::value< std::vector<double> >()->multitoken(), "wgs84 coordinates lng0 lat0 lng1 lat1")
       ("srs", po::value<std::string>(), "spatial reference system for input files")
       ("input", po::value< std::vector<std::string> >()->multitoken(), "list input files")
       ("verbose", "display additional information")
       ("inputdir", po::value<std::string>(), "input directory")
       ("filetype",  po::value<std::string>(), "file type")
       ;

   po::variables_map vm;

   try
   {
      po::store(po::parse_command_line(argc, argv, desc), vm);
      po::notify(vm);
   }
   catch (std::exception&)
   {
      std::cout << desc;
      return 4;
   }

   bool bVerbose = false;

   if (vm.count("verbose"))
   {
      bVerbose = true;
   }

   if ((vm.count("wgs84") && !vm.count("maxlod")) || (!vm.count("wgs84") && vm.count("maxlod")))
   {
      std::cout << "ERROR: option --wgs84 and --maxlod must be used together\n";
      return 1;
   }

   if (vm.count("wgs84") && vm.count("maxlod"))
   {
      int lod = vm["maxlod"].as<int>();
      if (lod < 0 || lod>23)
      {
         std::cout << "ERROR: lod value in wrong range\n";
         return 1;
      }

      double lng0, lat0, lng1, lat1;

      std::vector<double> vecCoords= vm["wgs84"].as< std::vector<double> >();
      if (vecCoords.size() != 4)
      {
         std::cout << "ERROR: wrong number of wgs84 coordinates\n";
         return 1;
      }
      
      lng0 = vecCoords[0];
      lat0 = vecCoords[1];
      lng1 = vecCoords[2];
      lat1 = vecCoords[3];
      
      if (lng0 >= lng1) { std::cout << "error: lng1 must be greater than lng0\n"; return 0; }
      if (lat0 >= lat1) { std::cout << "error: lat1 must be greater than lat0\n"; return 0; }
      if (lod < 4) { std::cout << "error: level of detail must be atleast 4\n"; return 0; }


      _calcfromwgs84(lod, lng0, lat0, lng1, lat1);

      return 0;
   }
   else if (vm.count("srs") && vm.count("input"))
   {
      std::vector<std::string> vecFiles = vm["input"].as< std::vector<std::string> >();
      std::string srs = vm["srs"].as<std::string>();

      return _frominput(vecFiles, srs, bVerbose);
   }
   else if (vm.count("srs") && vm.count("inputdir") && vm.count("filetype"))
   {
      std::string srs = vm["srs"].as<std::string>();
      std::string inputdir = vm["inputdir"].as<std::string>();
      std::string filetype = vm["filetype"].as<std::string>();

      std::vector<std::string> vecFiles = FileSystem::GetFilesInDirectory(inputdir, filetype);

      if (vecFiles.size() == 0)
      {
         std::cout << "No files found!\n";
         return 1;
      }

      return _frominput(vecFiles, srs, bVerbose);
   }
   else
   {
      std::cout << desc << "\n";
      std::cout << "From input files: use --srs and --input together\n";
      std::cout << "From wgs84 coord: use --wgs84 and --maxlod together\n";
      return 1;
   }

   return 0;
}

//------------------------------------------------------------------------------

//------------------------------------------------------------------------------

int _frominput(const std::vector<std::string>& vecFiles, const std::string& srs, bool bVerbose)
{
   // create an array of Dataset info for parallel access.
   DataSetInfo* pDataset = new DataSetInfo[vecFiles.size()];

  
   if (StringUtils::Left(srs, 5) != "EPSG:")
   {
      std::cout << "Error: only srs starting with EPSG: are currently supported";
      return 1;
   }

   int epsg = atoi(srs.c_str()+5);
   std::cout << "SRS epsg-code: " << epsg << "\n";

   if (!ProcessingUtils::init_gdal())
   {
      std::cout << "Warning: gdal-data directory not found. Ouput may be wrong!\n";
   }   

   boost::shared_ptr<CoordinateTransformation> qCT;
   qCT = boost::shared_ptr<CoordinateTransformation>(new CoordinateTransformation(epsg, 3785));

   clock_t t0,t1;
   t0 = clock();

   for (int i=0;i<(int)vecFiles.size();i++)
   {
      ProcessingUtils::RetrieveDatasetInfo(vecFiles[i], qCT.get(), &pDataset[i], bVerbose);
   }

   // at this point we finished calculating all the boundaries of all datasets, now
   // calculate the min/max

   double total_dest_ulx = 1e20;
   double total_dest_lry = 1e20;
   double total_dest_lrx = -1e20;
   double total_dest_uly = -1e20;
   double pixelsize = 1e20;

   for (size_t i=0;i<vecFiles.size();i++)
   {
      if (pDataset[i].bGood)
      {
         total_dest_ulx = math::Min<double>(pDataset[i].dest_ulx, total_dest_ulx);
         total_dest_lry = math::Min<double>(pDataset[i].dest_lry, total_dest_lry);
         total_dest_lrx = math::Max<double>(pDataset[i].dest_lrx, total_dest_lrx);
         total_dest_uly = math::Max<double>(pDataset[i].dest_uly, total_dest_uly);
         pixelsize      = math::Min<double>(pDataset[i].pixelsize, pixelsize);
      }
   }

   t1 = clock();

   std::cout << "GATHERED BOUNDARY (Mercator):\n";
   std::cout.precision(16);
   std::cout << "        ulx: " << total_dest_ulx << "\n";
   std::cout << "        lry: " << total_dest_lry << "\n";
   std::cout << "        lrx: " << total_dest_lrx << "\n";
   std::cout << "        uly: " << total_dest_uly << "\n";
   std::cout << "BOUNDARY in WGS84:\n";


   MercatorQuadtree* pQuadtree = new MercatorQuadtree();
   double x,y;

   double lng0, lat0, lng1, lat1;
   x = total_dest_ulx; y = total_dest_lry;
   pQuadtree->MercatorToWGS84(x, y); 
   lng0 = x; lat0 = y;
   std::cout << "       lng0: " << x << "\n";  
   std::cout << "       lat0: " << y << "\n";
   x = total_dest_lrx; y = total_dest_uly;
   pQuadtree->MercatorToWGS84(x, y);
   lng1 = x; lat1 = y;
   std::cout << "       lng1: " << x << "\n";
   std::cout << "       lat1: " << y << "\n";
   std::cout << " pixelsize : " << pixelsize * 6378137.0 << " m\n"; 

   for (int i=1;i<23;i++)
   {
      std::cout << "LEVEL OF DETAIL " << i << ": ";
      _calcfromwgs84(i, lng0, lat0, lng1, lat1);
   }

   delete pQuadtree;

   std::cout << "calculated in: " << double(t1-t0)/double(CLOCKS_PER_SEC) << " s \n";

   delete[] pDataset;

   ProcessingUtils::exit_gdal();

   return 0;
}

//------------------------------------------------------------------------------

void _calcfromwgs84(int lod, double lng0, double lat0, double lng1, double lat1)
{
   MercatorQuadtree* pQuadtree = new MercatorQuadtree();
   
   int64 px0, py0, px1, py1, tx0, ty0, tx1, ty1;
   pQuadtree->WGS84ToPixel(lng0, lat0, lod, px0, py1);
   pQuadtree->WGS84ToPixel(lng1, lat1, lod, px1, py0);
   
   //std::cout << "Number of pixels in specified range: " << (px1-px0+1)*(py1-py0+1) << "\n";
   
   pQuadtree->PixelToTileCoord(px0, py0, tx0, ty0);
   pQuadtree->PixelToTileCoord(px1, py1, tx1, ty1);
   
   std::cout << "Tile Coords: (" << tx0 << ", " << ty0 << ")-(" << tx1 << ", " << ty1 << ")\n";
   
   delete pQuadtree;

}