/*
 * Copyright 2010 Christian Costa
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * Oracle LGPL Disclaimer: For the avoidance of doubt, except that if any license choice
 * other than GPL or LGPL is available it will apply instead, Oracle elects to use only
 * the Lesser General Public License version 2.1 (LGPLv2) at this time for any software where
 * a choice of LGPL license versions is made available with the language indicating
 * that LGPLv2 or any later version may be used, or where a choice of which version
 * of the LGPL is applied is otherwise unspecified.
 */

#include "d3dx9.h"

#ifndef __D3DX9SHAPE_H__
#define __D3DX9SHAPE_H__

#ifdef __cplusplus
extern "C" {
#endif

HRESULT WINAPI D3DXCreateBox(struct IDirect3DDevice9 *device, float width, float height,
        float depth, struct ID3DXMesh **mesh, struct ID3DXBuffer **adjacency);
HRESULT WINAPI D3DXCreateCylinder(struct IDirect3DDevice9 *device, float radius1, float radius2,
        float length, UINT slices, UINT stacks, struct ID3DXMesh **mesh, struct ID3DXBuffer **adjacency);
HRESULT WINAPI D3DXCreateSphere(struct IDirect3DDevice9 *device, float radius, UINT slices,
        UINT stacks, struct ID3DXMesh **mesh, struct ID3DXBuffer **adjacency);
HRESULT WINAPI D3DXCreateTeapot(struct IDirect3DDevice9 *device,
        struct ID3DXMesh **mesh, struct ID3DXBuffer **adjacency);
HRESULT WINAPI D3DXCreateTextA(struct IDirect3DDevice9 *device, HDC hdc, const char *text, float deviation,
        float extrusion, struct ID3DXMesh **mesh, struct ID3DXBuffer **adjacency, GLYPHMETRICSFLOAT *glyphmetrics);
HRESULT WINAPI D3DXCreateTextW(struct IDirect3DDevice9 *device, HDC hdc, const WCHAR *text, float deviation,
        FLOAT extrusion, struct ID3DXMesh **mesh, struct ID3DXBuffer **adjacency, GLYPHMETRICSFLOAT *glyphmetrics);
#define D3DXCreateText WINELIB_NAME_AW(D3DXCreateText)

#ifdef __cplusplus
}
#endif

#endif /* __D3DX9SHAPE_H__ */
