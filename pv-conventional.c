/* PV - phase vocoder : pv-conventional.c
 * Copyright (C) 2007 Kengo Ichiki <kichiki@users.sourceforge.net>
 * $Id: pv-conventional.c,v 1.3 2007/02/17 05:31:49 kichiki Exp $
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

// FFTW library
#include <fftw3.h>
// half-complex format handling routines
#include "hc.h"
#include "fft.h" // windowing()

// libsndfile
#include <sndfile.h>
#include "snd.h"

// esd sound device
#include <esd.h>
#include "esd-wrapper.h"


/* standard phase vocoder
 * Ref: J.Laroche and M.Dolson (1999)
 */
void pv (const char *file, const char *outfile,
	 double rate, long len, long hop_out,
	 int flag_window)
{
  long hop_in;
  hop_in = (long)((double)hop_out * rate);


  double twopi = 2.0 * M_PI;

  int i;
  int k;

  // open wav file
  long read_status;
  // libsndfile version
  SNDFILE *sf = NULL;
  SF_INFO sfinfo;
  memset (&sfinfo, 0, sizeof (sfinfo));
  sf = sf_open (file, SFM_READ, &sfinfo);
  if (sf == NULL)
    {
      fprintf (stderr, "fail to open %s\n", file);
      exit (1);
    }
  sndfile_print_info (&sfinfo);


  /* allocate buffers  */
  double * left = NULL;
  double * right = NULL;
  left  = (double *) malloc (sizeof (double) * len);
  right = (double *) malloc (sizeof (double) * len);


  /* open sound device */
  int status;
  int esd = 0; // for compiler warning...
  SNDFILE *sfout = NULL;
  SF_INFO sfout_info;
  if (outfile == NULL)
    {
      esd = esd_init_16_stereo_strem_play (sfinfo.samplerate);
    }
  else
    {
      sfout = sndfile_open_for_write (&sfout_info,
				      outfile,
				      sfinfo.samplerate,
				      sfinfo.channels);
      if (sfout == NULL)
	{
	  fprintf (stderr, "fail to open file %s\n", outfile);
	  exit (1);
	}
    }
  long out_frames = 0;


  /* initialization plan for FFTW  */
  double *time = NULL;
  double *freq = NULL;
  time = (double *)fftw_malloc (len * sizeof(double));
  freq = (double *)fftw_malloc (len * sizeof(double));
  fftw_plan plan;
  plan = fftw_plan_r2r_1d (len, time, freq, FFTW_R2HC, FFTW_ESTIMATE);

  double *t_out = NULL;
  double *f_out = NULL;
  f_out = (double *)fftw_malloc (len * sizeof(double));
  t_out = (double *)fftw_malloc (len * sizeof(double));
  fftw_plan plan_inv;
  plan_inv = fftw_plan_r2r_1d (len, f_out, t_out,
			       FFTW_HC2R, FFTW_ESTIMATE);

  double *amp = NULL;
  double *ph_in = NULL;
  amp = (double *)malloc (((len/2)+1) * sizeof(double));
  ph_in     = (double *)malloc (((len/2)+1) * sizeof(double));

  double *l_ph_out = NULL;
  double *r_ph_out = NULL;
  l_ph_out    = (double *)malloc (((len/2)+1) * sizeof(double));
  r_ph_out    = (double *)malloc (((len/2)+1) * sizeof(double));

  double *l_ph_in_old = NULL;
  double *r_ph_in_old = NULL;
  l_ph_in_old = (double *)malloc (((len/2)+1) * sizeof(double));
  r_ph_in_old = (double *)malloc (((len/2)+1) * sizeof(double));
  for (i = 0; i < (len/2)+1; i ++)
    {
      ph_in [i]  = 0.0;
      l_ph_out [i] = 0.0;
      r_ph_out [i] = 0.0;
      l_ph_in_old [i] = 0.0;
      r_ph_in_old [i] = 0.0;
    }

  double *l_out = NULL;
  double *r_out = NULL;
  l_out = (double *) malloc ((hop_out + len) * sizeof(double));
  r_out = (double *) malloc ((hop_out + len) * sizeof(double));
  for (i = 0; i < (hop_out + len); i ++)
    {
      l_out [i] = 0.0;
      r_out [i] = 0.0;
    }

  // expected frequency
  double * omega = NULL;
  omega = (double *) malloc (((len/2)+1) * sizeof(double));
  for (k = 0; k < (len/2)+1; k ++)
    {
      omega [k] = twopi * (double)k / (double)len;
    }


  // read the first frame
  read_status = sndfile_read (sf, sfinfo, left, right, len);
  if (read_status != len)
    {
      exit (1);
    }

  int flag_ph = 0;
  for (;;)
    {
      // left channel
      apply_FFT (len, left, flag_window, plan, time, freq, 0.5, amp, ph_in);
      if (flag_ph == 0)
	{
	  // initialize phase
	  for (k = 0; k < (len/2)+1; k ++)
	    {
	      l_ph_out [k] = ph_in [k] * (double)hop_out / (double)hop_in;
	      //l_ph_out [k] = ph_in [k];

	      // backup for the next step
	      l_ph_in_old [k] = ph_in [k];
	    }
	  //flag_ph = 1; // right channel is in the following!
	}
      else
	{
	  // only for imag components who have phase
	  for (k = 1; k < ((len+1)/2); k ++)
	    {
	      double dphi;
	      dphi = ph_in [k] - l_ph_in_old [k]
		- omega [k] * (double)hop_in;
	      for (; dphi >= M_PI; dphi -= twopi);
	      for (; dphi < -M_PI; dphi += twopi);

	      l_ph_out [k] += dphi * (double)hop_out / (double)hop_in
		+ omega [k] * (double)hop_out;

	      l_ph_in_old [k] = ph_in [k];
	    }
	}
      polar_to_HC (len, amp, l_ph_out, 0, f_out);
      fftw_execute (plan_inv);
      // scale by len and windowing
      windowing (len, t_out, flag_window, (double)len, t_out);
      // superimpose
      for (i = 0; i < len; i ++)
	{
	  l_out [hop_out + i] += t_out [i];
	}

      // right channel
      apply_FFT (len, right, flag_window, plan, time, freq, 0.5, amp, ph_in);
      if (flag_ph == 0)
	{
	  // initialize phase
	  for (k = 0; k < (len/2)+1; k ++)
	    {
	      r_ph_out [k] = ph_in [k] * (double)hop_out / (double)hop_in;
	      //r_ph_out [k] = ph_in [k];

	      // backup for the next step
	      r_ph_in_old [k] = ph_in [k];
	    }
	  flag_ph = 1;
	}
      else
	{
	  // only for imag components who have phase
	  for (k = 1; k < ((len+1)/2); k ++)
	    {
	      double dphi;
	      dphi = ph_in [k] - r_ph_in_old [k]
		- omega [k] * (double)hop_in;
	      for (; dphi >= M_PI; dphi -= twopi);
	      for (; dphi < -M_PI; dphi += twopi);

	      r_ph_out [k] += dphi * (double)hop_out / (double)hop_in
		+ omega [k] * (double)hop_out;

	      r_ph_in_old [k] = ph_in [k];
	    }
	}
      polar_to_HC (len, amp, r_ph_out, 0, f_out);
      fftw_execute (plan_inv);
      // scale by len and windowing
      windowing (len, t_out, flag_window, (double)len, t_out);
      // superimpose
      for (i = 0; i < len; i ++)
	{
	  r_out [hop_out + i] += t_out [i];
	}

      /* output */
      if (outfile == NULL)
	{
	  status = esd_write (esd, l_out, r_out, hop_out);
	}
      else
	{
	  status = sndfile_write (sfout, sfout_info, l_out, r_out, hop_out);
	  out_frames += status;
	}


      /* shift acc_out by hop_out */
      for (i = 0; i < len; i ++)
	{
	  l_out [i] = l_out [i + hop_out];
	  r_out [i] = r_out [i + hop_out];
	}
      for (i = len; i < len + hop_out; i ++)
	{
	  l_out [i] = 0.0;
	  r_out [i] = 0.0;
	}


      /* for the next step */
      for (i = 0; i < (len - hop_in); i ++)
	{
	  left  [i]  = left  [i + hop_in];
	  right  [i] = right  [i + hop_in];
	}

      /* read next segment */
      read_status = sndfile_read (sf, sfinfo,
				  left  + len - hop_in,
				  right + len - hop_in,
				  hop_in);
      if (read_status != hop_in)
	{
	  // most likely, it is EOF.
	  break;
	}
    }


  free (left);
  free (right);

  free (time);
  free (freq);
  fftw_destroy_plan (plan);

  free (t_out);
  free (f_out);
  fftw_destroy_plan (plan_inv);

  free (amp);
  free (ph_in);

  free (l_ph_out);
  free (r_ph_out);
  free (l_ph_in_old);
  free (r_ph_in_old);

  free (l_out);
  free (r_out);

  free (omega);

  sf_close (sf) ;
  if (outfile == NULL) esd_close (esd);
  else
    {
      // frames left in l_out[] and r_out[]
      status = sndfile_write (sfout, sfout_info, l_out, r_out, len);
      out_frames += status;

      // set frames
      sfout_info.frames = out_frames;
      sf_write_sync (sfout);
      sf_close (sfout);
    }
}