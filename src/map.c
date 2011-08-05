#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "map.h"
#include "layer.h"
#include "filter.h"
#include "style.h"
#include "util.h"


#define SIMPLET_SLIPPY_SIZE 256
#define SIMPLET_MERC_LENGTH 40075016.68

simplet_map_t*
simplet_map_new(){
  simplet_error_init();
  simplet_map_t *map;
  if(!(map = malloc(sizeof(*map))))
    return NULL;

  if(!(map->layers = simplet_list_new())){
    free(map);
    return NULL;
  }

  map->bounds       = NULL;
  map->proj         = NULL;
  map->_ctx         = NULL;
  map->error.status = SIMPLET_OK;
  map->height       = 0;
  map->width        = 0;
  map->valid        = SIMPLET_OK;
  return map;
}

void
simplet_map_free(simplet_map_t *map){
  if(map->bounds)
    simplet_bounds_free(map->bounds);

  if(map->layers) {
    simplet_list_set_item_free(map->layers,simplet_layer_vfree);
    simplet_list_free(map->layers);
  }

  if(map->_ctx)
    cairo_destroy(map->_ctx);

  if(map->proj)
    OSRRelease(map->proj);

  free(map);
}

static simplet_status_t
simplet_map_error(simplet_map_t *map, simplet_status_t err, const char* msg){
  simplet_set_error(&map->error, err, msg);
  return map->valid = err;
}

simplet_status_t
simplet_map_set_srs(simplet_map_t *map, const char *proj){
  if(map->proj)
    OSRRelease(map->proj);

  if(!(map->proj = OSRNewSpatialReference(NULL)))
    return simplet_map_error(map, SIMPLET_OGR_ERR, "could not assign spatial ref");

  if(OSRSetFromUserInput(map->proj, proj) != OGRERR_NONE)
    return simplet_map_error(map, SIMPLET_OGR_ERR, "bad projection string");

  return SIMPLET_OK;
}

void
simplet_map_get_srs(simplet_map_t *map, char **srs){
  OSRExportToProj4(map->proj, srs);
}

simplet_status_t
simplet_map_set_size(simplet_map_t *map, int width, int height){
  map->height = height;
  map->width  = width;
  return SIMPLET_OK;
}

simplet_status_t
simplet_map_set_bounds(simplet_map_t *map, double maxx, double maxy, double minx, double miny){
  if(map->bounds)
    simplet_bounds_free(map->bounds);

  if(!(map->bounds = simplet_bounds_new()))
    return simplet_map_error(map, SIMPLET_OOM, "couldn't create bounds");

  simplet_bounds_extend(map->bounds, maxx, maxy);
  simplet_bounds_extend(map->bounds, minx, miny);
  return SIMPLET_OK;
}


simplet_status_t
simplet_map_set_slippy(simplet_map_t *map, unsigned int x, unsigned int y, unsigned int z){
  simplet_map_set_size(map, SIMPLET_SLIPPY_SIZE, SIMPLET_SLIPPY_SIZE);

  if(!simplet_map_set_srs(map, SIMPLET_MERCATOR))
    return simplet_map_error(map, SIMPLET_OGR_ERR, "couldn't set slippy projection");

  double zfactor, length, origin;
  zfactor = pow(2.0, z);
  length  = SIMPLET_MERC_LENGTH / zfactor;
  origin  = SIMPLET_MERC_LENGTH / 2;

  if(!simplet_map_set_bounds(map, (x + 1) * length - origin,
                                  origin - (y + 1) * length,
                                  x * length - origin,
                                  origin - y * length))
    return simplet_map_error(map, SIMPLET_OOM, "out of memory setting bounds");

  return SIMPLET_OK;
}

simplet_status_t
simplet_map_get_status(simplet_map_t *map){
  return map->error.status;
}

const char*
simplet_map_status_to_string(simplet_map_t *map){
  return (const char*) map->error.msg;
}

simplet_status_t
simplet_map_is_valid(simplet_map_t *map){
  if(map->valid != SIMPLET_OK)
    return SIMPLET_ERR;

  if(!map->bounds)
    return SIMPLET_ERR;

  if(!map->proj)
    return SIMPLET_ERR;

  if(!map->height)
    return SIMPLET_ERR;

  if(!map->width)
    return SIMPLET_ERR;

  if(!map->layers->tail)
    return SIMPLET_ERR;

  return SIMPLET_OK;
}

cairo_surface_t *
simplet_map_build_surface(simplet_map_t *map){
  if(simplet_map_is_valid(map) == SIMPLET_ERR)
    return NULL;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, map->width, map->height);
  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    return NULL;

  cairo_t *ctx = cairo_create(surface);
  map->_ctx = ctx;
  simplet_listiter_t *iter = simplet_get_list_iter(map->layers);
  simplet_layer_t *layer;
  simplet_status_t err;

  OGRRegisterAll();
  while((layer = simplet_list_next(iter))){
    err = simplet_layer_process(layer, map);
    if(err != SIMPLET_OK) {
      simplet_list_iter_free(iter);
      simplet_map_error(map, err, "error in rendering");
      break;
    }
  }

  return surface;
}

void
simplet_map_close_surface(simplet_map_t *map, cairo_surface_t *surface){
  cairo_destroy(map->_ctx);
  map->_ctx = NULL;
  cairo_surface_destroy(surface);
}

void
simplet_map_render_to_stream(simplet_map_t *map, void *stream,
  cairo_status_t (*cb)(void *closure, const unsigned char *data, unsigned int length)){

  cairo_surface_t *surface;
  if(!(surface = simplet_map_build_surface(map))) return;

  if(cairo_surface_write_to_png_stream(surface, cb, stream) != CAIRO_STATUS_SUCCESS)
    simplet_map_error(map, SIMPLET_CAIRO_ERR, cairo_status_to_string(cairo_status(map->_ctx)));

  simplet_map_close_surface(map, surface);
}

void
simplet_map_render_to_png(simplet_map_t *map, const char *path){

  cairo_surface_t *surface;
  if(!(surface = simplet_map_build_surface(map))) return;

  if(cairo_surface_write_to_png(surface, path) != CAIRO_STATUS_SUCCESS)
    simplet_map_error(map, SIMPLET_CAIRO_ERR, cairo_status_to_string(cairo_status(map->_ctx)));

  simplet_map_close_surface(map, surface);
}

