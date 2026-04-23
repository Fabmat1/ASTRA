

%
% only used for the mass ratio in case of a derived vrad
define dummy_fit(lo, hi, par)
{
  return 1.0;
}
add_slang_function("dummy", "q");

%
% to also model interstellar CaII and NaI lines
#ifeval 0
initialize_interstellar(; datasets=len_sets);
fit_fun("stellar*telluric*interstellar");
#endif

%
% location of default linelist
variable ll_default_path;
if(stat_file(ISISSCRIPTS_REFPATH+"/templates/linelist.txt")!=NULL)
  ll_default_path = ISISSCRIPTS_REFPATH+"/templates/linelist.txt";

%
delete_data(all_data);
variable mjd = NULL;
_for id(1, len_sets, 1) % loop over datasets
{
  if(stat_file(sprintf("%sfitsfiles/d%d_spectrum.fits.gz", wd, id))!=NULL)
    () = define_counts(fits_read_table(sprintf("%sfitsfiles/d%d_spectrum.fits.gz", wd, id)));
  else
  {
    % -------------
    % read spectra:
    if(spectype[id-1]=="ASCII_with_2_columns"){
      variable l, f, err;
      % ASCII format:
      variable t = ascii_read_table(specs[id-1], [{"%F"}, {"%F"}]; startline=0);
      % t = csv_readcol(specs[id-1]; comment="#", delim=',');
      l = t.col1; f = t.col2; __uninitialize(&t);
      % order by wavelength
      variable ind = array_sort(l); l = l[ind]; f = f[ind];
      % to remove spectrum red and blue edges (in Angstrom)
      if(wave_trim[id-1] != NULL){
        ind = where(l>=min(wave_trim[id-1]) and l<max(wave_trim[id-1]));
        l = l[ind];
        f = f[ind];
      }
      % to remove leading and trailing zeros
      ind = where(f>0); l = l[ind]; f = f[ind];
      vmessage("Removing cosmics - check afterwards that emission lines are not clipped/removed");
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=50, verbose=0);
      remove_cosmics(l,f; cosmics=3, range=25, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      (l,f) = optimize_wavegrid(l,f,res_offset[id-1],res_slope[id-1]);
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      f /= median(f); % to have flux values at the order of one
      variable dp, snr;
      dp = max([700,length(l)/10]);
%      snr = snr_curve(l, f, "der_snr"; data_points=300);
      snr = snr_curve(l, f, "gauss"; data_points=dp);
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
      err = snr.noise;
      if(max(l)<5800.){
        set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
        set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
        set_par(sprintf("telluric(1).d%d_barycorr",id),0,1);
      }
    }
    if(spectype[id-1]=="ASCII_with_3_columns"){
      variable l, f, err;
      % ASCII format:
      (l,f,err) = readcol(specs[id-1],1,2,3);
%      variable t = ascii_read_table(specs[id-1], [{"%F"}, {"%F"}, {"%F"}]; startline=0);
%      variable t = csv_readcol(specs[id-1]; comment="#", delim=',');
%      l = t.col1; f = t.col2; err = t.col3; __uninitialize(&t);
      % order by wavelength
      variable ind = array_sort(l); l = l[ind]; f = f[ind]; err = err[ind];
      % to remove spectrum red and blue edges (in Angstrom)
      if(wave_trim[id-1] != NULL){
        ind = where(l>=min(wave_trim[id-1]) and l<max(wave_trim[id-1]));
        l = l[ind];
        f = f[ind];
        err = err[ind];
      }
      % to remove leading and trailing zeros
      ind = where(f>0); l = l[ind]; f = f[ind]; err = err[ind];
      % to account for gaps
      variable temp;
      ind = where(err<=0, &temp); err[ind] = interpol(l[ind],l[temp],err[temp]); __uninitialize(&temp);
      vmessage("Removing cosmics - check afterwards that emission lines are not clipped/removed");
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=50, verbose=0);
      remove_cosmics(l,f; cosmics=3, range=25, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      variable snr = f / err;
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr[snr > 0]));
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      % to have flux values at the order of one
      temp = median(f); f /= temp; err /= temp; __uninitialize(&temp);
      if(max(l)<5800.){
        set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
        set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
        set_par(sprintf("telluric(1).d%d_barycorr",id),0,1);
      }
    }
    if(spectype[id-1]=="GIRAFFE"){
      % GIRAFFE pipeline format:
      vmessage("%s", fits_read_key(specs[id-1], "Object"));
      variable temp = fits_read_table(specs[id-1]);
      l = temp.wave[0,*], f = temp.flux_reduced[0,*], err = temp.err_reduced[0,*];
      l *= 10.; % conversion from nm to Angstroem
      __uninitialize(&temp);
      ind = where(f>0); l = l[ind]; f = f[ind]; err = err[ind]; % to remove leading and trailing zeros
      if(fits_key_exists(specs[id-1]+"[0]", "SPEC_RES"))
        res_offset[id-1] = fits_read_key(specs[id-1]+"[0]", "SPEC_RES");
      res_slope[id-1] = 0.;
      variable barycorr = fits_read_key(specs[id-1]+"[0]", "BARYCORR");
      l *= (1.+barycorr/299792.458);
      temp = median(f); f /= temp; err /= temp; __uninitialize(&temp); % to have flux values at the order of one
    }
    if(spectype[id-1]=="MUSE"){
      % MUSE pipeline format:
      variable CRPIX1, CRVAL1, CDELT1, NAXIS1, mjd;
      (CRPIX1, CRVAL1, CDELT1, NAXIS1, mjd) = fits_read_key(specs[id-1],
        "CRPIX1", "CRVAL1", "CDELT1", "NAXIS1", "MJD-OBS");
      l = CRVAL1 + ([1:NAXIS1:1]-CRPIX1) * CDELT1;
      f = fits_read_img(specs[id-1]+"[0]");
%      err = fits_read_img(specs[id-1]+"[1]");
      f = f[[0:length(l)-1:1]];
%      err = err[[0:length(l)-1:1]];
      % to remove leading and trailing zeros
      ind = where(f>0); l = l[ind]; f = f[ind];
%      err = err[ind];
      f /= median(f); % to have flux values at the order of one
%      err /= median(f);
      variable dp = max([700,length(l)/10]);
%      snr = snr_curve(l, f, "der_snr"; data_points=300);
      snr = snr_curve(l, f, "gauss"; data_points=dp);
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
      err = snr.noise;
      % suggested by Steven Hämmerich
      res_offset[id-1] = -432.72712075; % 3014.;
      res_slope[id-1] = 0.44530811; % 0.;
      set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
      set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
      set_par(sprintf("telluric(1).d%d_barycorr",id),0,1);
    }
    if(spectype[id-1]=="SALT"){
      variable s = fits_read_img(specs[id-1]);
      l = s[0,[0:array_shape(s)[1]-1]];
      f = s[1,[0:array_shape(s)[1]-1]];
      % to remove leading and trailing zeros
      ind = where(f>0); l = l[ind]; f = f[ind];
      % to have flux values at the order of one
      f /= median(f);
      % resample to a proper wavelength grid, given res
      (l,f) = optimize_wavegrid(l,f,res_offset[id-1],res_slope[id-1]);
      % estimate SNR
      variable dp = max([700,length(l)/10]);
%      snr = snr_curve(l, f, "der_snr"; data_points=dp);
      snr = snr_curve(l, f, "gauss"; data_points=dp);
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
      err = snr.noise;
      if(max(l)<5800.){
        set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
        set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
        set_par(sprintf("telluric(1).d%d_barycorr",id),0,1);
      }
    }
    if(spectype[id-1]=="IRAF"){
      if (fits_key_exists(specs[id-1], "CD1_1"))
        (CRPIX1, CRVAL1, CDELT1, NAXIS1) = fits_read_key(specs[id-1], "CRPIX1", "CRVAL1", "CD1_1", "NAXIS1");
      else
        (CRPIX1, CRVAL1, CDELT1, NAXIS1) = fits_read_key(specs[id-1], "CRPIX1", "CRVAL1", "CDELT1", "NAXIS1");
      l = CRVAL1 + ([1:NAXIS1:1]-CRPIX1) * CDELT1;
      % if wavelengths are in natural logarithm -.-
      if(median(l)<10.){
        variable e = 2.718281828459045;
        l = e^l;
      }
      f = fits_read_img(specs[id-1]+"[0]");
      f = f[[0:length(l)-1:1]];

      % in case a crazy person set up the fits
      if(median(f)<0) f = 10^f;

      % to remove leading and trailing zeros
      ind = where(f>0); l = l[ind]; f = f[ind];
      ind = where(3704 < l < 7300); l = l[ind]; f = f[ind];
      f /= median(f); % to have flux values at the order of one
      (l,f) = optimize_wavegrid(l,f,res_offset[id-1],res_slope[id-1]);
      variable dp = max([300,length(l)/10]);
%      snr = snr_curve(l, f, "der_snr"; data_points=dp);
      snr = snr_curve(l, f, "gauss"; data_points=dp);
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
      err = snr.noise;
      variable barycorr = fits_read_key(specs[id-1]+"[0]", "VHELIO");
      if(barycorr==NULL) barycorr = fits_read_key(specs[id-1]+"[0]", "BVCOR");
      if(barycorr==NULL) barycorr = fits_read_key(specs[id-1]+"[0]", "VBARY");
      if(barycorr!=NULL)
      {
        l *= (1.+barycorr/299792.458);
        set_par(sprintf("telluric(1).d%d_barycorr",id),barycorr,0);
        vmessage(sprintf("Applied heliocentric correction of %.3f km/s.", barycorr));
      }
      else{
        vmessage("Could not read header 'VHELIO' for heliocentric correction.");
        barycorr = 0.;
      }
      if(max(l)<5800.){
        set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
        set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
        set_par(sprintf("telluric(1).d%d_barycorr",id),barycorr,1);
      }
    }
    if(spectype[id-1]=="INT"){
      (CRPIX1, CRVAL1, CDELT1, NAXIS1, mjd) = fits_read_key(specs[id-1],
        "CRPIX1", "CRVAL1", "CD1_1", "NAXIS1", "MJD-OBS");
      l = CRVAL1 + ([1:NAXIS1:1]-CRPIX1) * CDELT1;
      f = fits_read_img(specs[id-1]+"[0]");
      f = f[[0:length(l)-1:1]];
      % to remove leading and trailing zeros
      ind = where(f>0); l = l[ind]; f = f[ind];
      % removed edges of the spectrum
      ind = where(3580 < l < 7050); l = l[ind]; f = f[ind];
      f /= median(f); % to have flux values at the order of one
      variable dp = max([400,length(l)/10]);
%      snr = snr_curve(l, f, "der_snr"; data_points=dp);
      snr = snr_curve(l, f, "gauss"; data_points=dp);
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
      err = snr.noise;
      if(max(l)<5800.){
        set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
        set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
        set_par(sprintf("telluric(1).d%d_barycorr",id),0,1);
      }
    }
    if(spectype[id-1]=="FEROS_phase3"){
      % FEROS phase3 format:
      temp = fits_read_table(specs[id-1]);
      l = temp.wave[0,*], f = temp.flux[0,*];
      ind = where(3680 < l < 9200); l = l[ind]; f = f[ind];
      vmessage("Removing cosmics - check afterwards that emission lines are not clipped/removed");
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=50, verbose=0);
      remove_cosmics(l,f; cosmics=3, range=25, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      (l,f) = optimize_wavegrid(l,f,res_offset[id-1],res_slope[id-1]);
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      if(stat_file(ISISSCRIPTS_REFPATH+"/refdata/FEROS_response_curve.fits.gz")!=NULL){
        variable corr = fits_read_table(ISISSCRIPTS_REFPATH+"/refdata/FEROS_response_curve.fits.gz");
        f /= interpol(l, corr.l, corr.f);
        __uninitialize(&corr);
      }
      f /= median(f); % to have flux values at the order of one
      variable dp = max([700,length(l)/10]);
%      snr = snr_curve(l, f, "der_snr"; data_points=dp);
      snr = snr_curve(l, f, "gauss"; data_points=dp);
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
      err = snr.noise;
      barycorr = fits_read_key(specs[id-1]+"[0]", "HIERARCH ESO DRS BARYCORR");
      l *= (1.+barycorr/299792.458);
      set_par(sprintf("telluric(1).d%d_barycorr",id),barycorr,0);
      }
    if(spectype[id-1]=="FEROS"){
      % FEROS pipeline format:
      variable target;
      (CRPIX1, CRVAL1, CDELT1, NAXIS1, target) = fits_read_key(specs[id-1],
        "CRPIX1", "CRVAL1", "CDELT1", "NAXIS1", "HIERARCH ESO OBS TARG NAME");
      vmessage("%s", target);
      l = CRVAL1 + ([1:NAXIS1:1]-CRPIX1) * CDELT1; f = fits_read_img(specs[id-1]);
      % % FEROS phase 3 format:
      % variable temp = fits_read_table(specs[id-1]); l = temp.wave[0,*]; f = temp.flux[0,*]; __uninitialize(&temp);
      ind = where(3680 < l < 9200); l = l[ind]; f = f[ind];
      vmessage("Removing cosmics - check afterwards that emission lines are not clipped/removed");
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=50, verbose=0);
      remove_cosmics(l,f; cosmics=3, range=25, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      (l,f) = optimize_wavegrid(l,f,res_offset[id-1],res_slope[id-1]);
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      % l *= (1.+barycorr/299792.458); % barycentric correction is already applied in FEROS-MIDAS pipeline
      if(stat_file(ISISSCRIPTS_REFPATH+"/refdata/FEROS_response_curve.fits.gz")!=NULL){
        variable corr = fits_read_table(ISISSCRIPTS_REFPATH+"/refdata/FEROS_response_curve.fits.gz");
        f /= interpol(l, corr.l, corr.f);
        __uninitialize(&corr);
      }
      f /= median(f); % to have flux values at the order of one
      variable dp = max([700,length(l)/10]);
%      snr = snr_curve(l, f, "der_snr"; data_points=dp);
      snr = snr_curve(l, f, "gauss"; data_points=dp);
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
      err = snr.noise;
    }
    if(spectype[id-1]=="UVES"){
      % UVES pipeline format:
      if(fits_key_exists(specs[id-1]+"[0]", "Object"))
        vmessage("%s", fits_read_key(specs[id-1]+"[0]", "Object"));
      if(fits_key_exists(specs[id-1]+"[0]", "MJD-OBS"))
        mjd = fits_read_key(specs[id-1]+"[0]", "MJD-OBS");
      temp = fits_read_table(specs[id-1]);
      l = temp.wave[0,*], f = temp.flux[0,*], err = temp.err[0,*];
      __uninitialize(&temp);
      ind = where(f>0); l = l[ind]; f = f[ind]; err = err[ind]; % to remove leading and trailing zeros
%      ind = where(3680 < l < 9200); l = l[ind]; f = f[ind]; err = err[ind];
      if(fits_key_exists(specs[id-1]+"[0]", "SPEC_RES"))
        res_offset[id-1] = fits_read_key(specs[id-1]+"[0]", "SPEC_RES");
      res_slope[id-1] = 0.;
      vmessage("Removing cosmics - check afterwards that emission lines are not clipped/removed");
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=50, verbose=0);
      remove_cosmics(l,f; cosmics=3, range=25, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      variable l_old = l;
      (l,f) = optimize_wavegrid(l,f,res_offset[id-1],res_slope[id-1]);
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      % interpolate err
      if(length(l)!=length(l_old))
        err = interpol(l, l_old, err);
      % set to 1 to replace errors; they are often overestimated in phase3
      variable estimate_errors = 0;
      if(estimate_errors){
        variable dp = max([300,length(l)/20]);
        % snr = snr_curve(l, f, "der_snr"; data_points=dp);
        snr = snr_curve(l, f, "gauss"; data_points=dp);
        vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
        err = snr.noise;
      }
      barycorr = fits_read_key(specs[id-1]+"[0]", "HIERARCH ESO QC VRAD BARYCOR");
      if(barycorr==NULL) barycorr = fits_read_key(specs[id-1]+"[0]", "HIERARCH ESO QC VRAD HELICOR");
      if(barycorr!=NULL){
        l *= (1.+barycorr/299792.458);
        set_par(sprintf("telluric(1).d%d_barycorr",id),barycorr,0);
      }
      else{
        vmessage("WARNING: did not find BARYCOR");
      }
      % switch off telluric lines if there are none in this spectral range
      if(max(l)<5800.){
        set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
        set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
      }
      temp = median(f); f /= temp; err /= temp; __uninitialize(&temp); % to have flux values at the order of one
    }
    if(spectype[id-1]=="UVES_esoreflex"){
      % XSHOOTER pipeline format:
      if(fits_key_exists(specs[id-1]+"[0]", "Object"))
        vmessage("%s", fits_read_key(specs[id-1], "Object"));
      if(fits_key_exists(specs[id-1]+"[0]", "MJD-OBS"))
        mjd = fits_read_key(specs[id-1]+"[0]", "MJD-OBS");
      f = fits_read_img(specs[id-1]+"[0]");
      (CRPIX1, CRVAL1, CDELT1, NAXIS1) = fits_read_key(specs[id-1], "CRPIX1", "CRVAL1", "CDELT1", "NAXIS1");
      l = CRVAL1 + ([1:NAXIS1:1]-CRPIX1) * CDELT1;
      % conversion from nm to Angstroem
      if(max(l)<3000) l *= 10.;
      if(min(l)>5000.)
        ind = where(5750 < l < 10000);
      else
        ind = where(3600 < l < 9300);
      l = l[ind]; f = f[ind];
      ind = where(f>0);
      l = l[ind]; f = f[ind];
      % to have flux values at the order of one
      temp = median(f); f /= temp; __uninitialize(&temp);
      % to account for gaps
      % ind = where(err<=0, &temp); err[ind] = interpol(l[ind],l[temp],err[temp]);
      % __uninitialize(&temp);
      vmessage("Removing cosmics - check afterwards that emission lines are not clipped/removed");
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=50, verbose=0);
      remove_cosmics(l,f; cosmics=3, range=25, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      variable dp = max([700,length(l)/10]);
%      snr = snr_curve(l, f, "der_snr"; data_points=dp);
      snr = snr_curve(l, f, "gauss"; data_points=dp);
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
      err = snr.noise;
      barycorr = fits_read_key(specs[id-1]+"[0]", "HIERARCH ESO QC VRAD BARYCOR");
      if(barycorr!=NULL){
        l *= (1.+barycorr/299792.458);
        set_par(sprintf("telluric(1).d%d_barycorr",id),barycorr,0);
      }
      else{
        vmessage("WARNING: did not find BARYCOR");
      }
      if(fits_key_exists(specs[id-1]+"[0]", "SPEC_RES"))
        res_offset[id-1] = fits_read_key(specs[id-1]+"[0]", "SPEC_RES");
      res_slope[id-1] = 0.;
      if(max(l)<5800.) % switch off telluric lines if there are none in this spectral range
      {
        set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
        set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
      }
    }
    if(spectype[id-1]=="XSHOOTER"){
      % XSHOOTER pipeline format:
      if(fits_key_exists(specs[id-1]+"[0]", "Object"))
        vmessage("%s", fits_read_key(specs[id-1]+"[0]", "Object"));
      if(fits_key_exists(specs[id-1]+"[0]", "MJD-OBS"))
        mjd = fits_read_key(specs[id-1]+"[0]", "MJD-OBS");
      temp = fits_read_table(specs[id-1]);
      l = temp.wave[0,*], f = temp.flux[0,*], err = temp.err[0,*];
      l *= 10.; % conversion from nm to Angstroem
      __uninitialize(&temp);
      if(min(l)>5000.)
        ind = where(5750 < l < 9300);
      else
        ind = where(3600 < l < 9300);
      l = l[ind]; f = f[ind]; err = err[ind];
      % to account for gaps
      ind = where(err<=0, &temp); err[ind] = interpol(l[ind],l[temp],err[temp]); __uninitialize(&temp);
      vmessage("Removing cosmics - check afterwards that emission lines are not clipped/removed");
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=50, verbose=0);
      remove_cosmics(l,f; cosmics=3, range=25, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      barycorr = fits_read_key(specs[id-1]+"[0]", "HIERARCH ESO QC VRAD BARYCOR");
      if(barycorr!=NULL){
        l *= (1.+barycorr/299792.458);
        set_par(sprintf("telluric(1).d%d_barycorr",id),barycorr,0);
      }
      else{
        vmessage("WARNING: did not find BARYCOR");
      }
      temp = median(f); f /= temp; err /= temp; __uninitialize(&temp); % to have flux values at the order of one
      if(fits_key_exists(specs[id-1]+"[0]", "SPEC_RES"))
        res_offset[id-1] = fits_read_key(specs[id-1]+"[0]", "SPEC_RES");
      res_slope[id-1] = 0.;
      if(max(l)<5800.) % switch off telluric lines if there are none in this spectral range
      {
        set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
        set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
      }
    }
    if(spectype[id-1]=="XSHOOTER_esoreflex"){
      % XSHOOTER pipeline format:
      if(fits_key_exists(specs[id-1]+"[0]", "Object"))
        vmessage("%s", fits_read_key(specs[id-1], "Object"));
      if(fits_key_exists(specs[id-1]+"[0]", "MJD-OBS"))
        mjd = fits_read_key(specs[id-1]+"[0]", "MJD-OBS");
      f = fits_read_img(specs[id-1]+"[0]");
      err = fits_read_img(specs[id-1]+"[1]");
      (CRPIX1, CRVAL1, CDELT1, NAXIS1) = fits_read_key(specs[id-1], "CRPIX1", "CRVAL1", "CDELT1", "NAXIS1");
      l = CRVAL1 + ([1:NAXIS1:1]-CRPIX1) * CDELT1;
      l *= 10.; % conversion from nm to Angstroem
      if(min(l)>5000.)
        ind = where(5750 < l < 10000);
      else
        ind = where(3600 < l < 9300);
      l = l[ind]; f = f[ind]; err = err[ind];
      % to account for gaps
      ind = where(err<=0, &temp); err[ind] = interpol(l[ind],l[temp],err[temp]); __uninitialize(&temp);
      vmessage("Removing cosmics - check afterwards that emission lines are not clipped/removed");
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=50, verbose=0);
      remove_cosmics(l,f; cosmics=3, range=25, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      barycorr = fits_read_key(specs[id-1]+"[0]", "HIERARCH ESO QC VRAD BARYCOR");
      if(barycorr!=NULL){
        l *= (1.+barycorr/299792.458);
        set_par(sprintf("telluric(1).d%d_barycorr",id),barycorr,0);
      }
      else{
        vmessage("WARNING: did not find BARYCOR");
      }
      temp = median(f); f /= temp; err /= temp; __uninitialize(&temp); % to have flux values at the order of one
      if(fits_key_exists(specs[id-1]+"[0]", "SPEC_RES"))
        res_offset[id-1] = fits_read_key(specs[id-1]+"[0]", "SPEC_RES");
      res_slope[id-1] = 0.;
      if(max(l)<5800.) % switch off telluric lines if there are none in this spectral range
      {
        set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
        set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
      }
    }
    if(spectype[id-1]=="LAMOST"){
      % LAMOST format
      f = fits_read_img(specs[id-1])[0,*];
      % row contains the inverse variance "ivar" -> err = 1/sqrt(ivar)
      err = 1./sqrt(fits_read_img(specs[id-1])[1,*]);
      l = fits_read_img(specs[id-1])[2,*]; l = vacuum_to_air(l); % row contains wavelength in vacuum
      % "andmask", see http://dr5.lamost.org/v3/doc/data-production-description
      variable mask = fits_read_img(specs[id-1])[3,*];
      vmessage("Mean signal-to-noise ratio of spectrum %d is %.0f.", id+1, mean((f/err)[where(isnan(f/err)==0)]));
      if(res_offset[id-1]==0 && res_slope[id-1]==0)
      {
        res_offset[id-1] = 0.;
        res_slope[id-1]  = 1800./5500.;
      }
      barycorr = fits_read_key(specs[id-1], "HELIO_RV");
      % apply barycentric correction if not yet done so
      if(fits_read_key(specs[id-1], "HELIO")!=1) l *= (1.+barycorr/299792.458);;
      set_par(sprintf("telluric(1).d%d_airmass", id), 0, 1; min=0, max=3);
      set_par(sprintf("telluric(1).d%d_pwv", id), 0, 1);
      set_par(sprintf("telluric(1).d%d_barycorr", id), barycorr, 1);
    }
    if(spectype[id-1]=="LAMOST_DR8"){
      % LAMOST DR8 format
      if(string_match(specs[id-1], "med-")==0){
        vmessage("Using LAMOST-LRS format.");
        temp = fits_read_table(specs[id-1]);
        f = temp.flux[0,*];
        l = temp.wavelength[0,*];
        err = 1./sqrt(temp.ivar[0,*]);
        % 'err' provided by LAMOST is not reliable -> better estimate it
        dp = max([700,length(l)/10]);
%        snr = snr_curve(l, f, "der_snr"; data_points=300);
        snr = snr_curve(l, f, "gauss"; data_points=dp);
        vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
        err = snr.noise;
        if(res_offset[id-1]==0 && res_slope[id-1]==0){
          res_offset[id-1] = 0.;
          res_slope[id-1]  = 1800./5500.;
        }
      }
      else{
        vmessage("Using LAMOST-MRS format.");
        temp = fits_read_table(specs[id-1]);
        l = temp.wavelength[0,*];
        f = temp.flux[0,*];
        err = 1./sqrt(temp.ivar[0,*]);
        temp = fits_read_table(specs[id-1]+"[2]");
        l = [l, temp.wavelength[0,*]];
        f = [f, temp.flux[0,*]];
        err = [err, 1./sqrt(temp.ivar[0,*])];
        if(res_offset[id-1]==0 && res_slope[id-1]==0){
          res_offset[id-1] = 7500.;
          res_slope[id-1]  = 0.;
        }
      }
      if(fits_read_key(specs[id-1]+"[0]", "VACUUM")==1) l = vacuum_to_air(l); % conversion to air wavelength
      ind = where(3600 < l < 8900); l = l[ind]; f = f[ind]; err = err[ind];
      barycorr = fits_read_key(specs[id-1]+"[0]", "HELIO_RV");
      % apply barycentric correction if not yet done so
      if(fits_read_key(specs[id-1]+"[0]", "HELIO")!=1) l *= (1.+barycorr/299792.458);;
      set_par(sprintf("telluric(1).d%d_airmass", id), 0, 1; min=0, max=3);
      set_par(sprintf("telluric(1).d%d_pwv", id), 0, 1);
      set_par(sprintf("telluric(1).d%d_barycorr", id), barycorr, 1);
    }
    if(spectype[id-1]=="4MOST"){
      % (preliminary?) 4MOST format for the first solar spectrum
      variable l, f, err, temp, ind, res_offset, res_slope, idx;
      variable CRPIX1, CRVAL1, CD1_1, NAXIS1;
      % blue, green, red
      variable idx_arms = [5, 3, 1];
      variable idx_arms = [5];
      variable iexp = 0;
      foreach idx (idx_arms)
      {
        variable ext_flux = sprintf("[%d]", idx);
        variable ext_ivar = sprintf("[%d]", idx+1);
        (CRPIX1, CRVAL1, CD1_1, NAXIS1) = fits_read_key(specs[id-1]+ext_flux,
         "CRPIX1", "CRVAL1", "CD1_1", "NAXIS1");
        l = CRVAL1 + ([1:NAXIS1:1]-CRPIX1) * CD1_1;
        f = fits_read_img(specs[id-1]+ext_flux);
        % select first exposure
        f = f[iexp,*]; % f = f[[0:length(l)-1:1]];
        err = fits_read_img(specs[id-1]+ext_ivar);
        err = err[iexp,*]; % err = err[[0:length(l)-1:1]];
        err = sqrt(1/err);
      }
      ind = where(isnan(f/err)==0); l = l[ind]; f = f[ind]; err = err[ind];
      variable snr_median = median((f/err)[ind]);
      % blue
      if(max(l)<5600.)
      {
        res_offset = -1727.41525511;
        res_slope = 1.76879236;
      }
      % green
      else if((max(l)<7300.) and (min(l)>5200.))
      {
        res_offset = -3056.07679;
        res_slope = 1.79399008;
      }
      % red
      else if(min(l)>6800.)
      {
        res_offset = -3016.18531;
        res_slope = 1.35867789;
      }
      % interpolate mean values for a joined spectrum
      else
      {
        res_offset = 3777.534470797719;
        res_slope = 0.579819477192359;
%        temp = get_4MOST_res(min(l),  max(l));
%        res_offset = temp[0];
%        res_slope = temp[1];
      }
    }
    if(spectype[id-1]=="SDSS"){
      % SDSS pipeline format (http://skyserver.sdss.org/dr16/en/tools/quicklook/summary.aspx?):
      temp = fits_read_table(specs[id-1]+"[COADD]");
      l = 10^temp.loglam; % conversion to linear scale
      l = vacuum_to_air(l); % conversion to air wavelength
      % l *= (1.+fits_read_key(specs[id-1]+"[0]","HELIO_RV")/299792.458); % heliocentric correction already done
      f = temp.flux;
      err = 1./sqrt(temp.ivar);
      % set spectral resolution
      variable pixel_width_in_angstroem = interpol(l, 0.5*(l[[1:]]+l[[:-2]]), l[[1:]]-l[[:-2]]);
      variable sigma_to_fwhm = 2*sqrt(2*log(2));
      % "wdisp" is the wavelength dispersion (sigma of fitted Gaussian) in units of number of pixel
      % -> temp is delta lambda in Angstroem
      temp = median( temp.wdisp*sigma_to_fwhm*pixel_width_in_angstroem );
      res_offset[id-1] = 0.;
      res_slope[id-1]  = 1./temp;
      % to have flux values at the order of one
      temp = median(f); f /= temp; err /= temp; __uninitialize(&temp);
      % see https://www.sdss.org/dr13/spectro/spectro_basics/
%      f *= 1e-17; err *= 1e-17;
      % switch off telluric lines because they are already removed
      set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
      set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
      set_par(sprintf("telluric(1).d%d_barycorr",id),0,1);
    }
    if(spectype[id-1]=="eBOSS"){
      % eBOSS pipeline format
      temp = fits_read_table(specs[id-1]+"[1]");
      l = 10^temp.loglam; % conversion to linear scale
      l = vacuum_to_air(l); % conversion to air wavelength
      f = temp.flux;
      err = 1./sqrt(temp.ivar);
      % set spectral resolution
      pixel_width_in_angstroem = interpol(l, 0.5*(l[[1:]]+l[[:-2]]), l[[1:]]-l[[:-2]]);
      sigma_to_fwhm = 2*sqrt(2*log(2));
      % "wdisp" is the wavelength dispersion (sigma of fitted Gaussian) in units of number of pixel
      % -> temp is delta lambda in Angstroem
      temp = median( temp.wdisp*sigma_to_fwhm*pixel_width_in_angstroem );
      res_offset[id-1] = 0.;
      res_slope[id-1]  = 1./temp;
      % to have flux values at the order of one
      temp = median(f); f /= temp; err /= temp; __uninitialize(&temp);
      % fluxes are given in units of 10^-17 erg/cm^2/s/Å
      % see https://www.sdss.org/dr13/spectro/spectro_basics/
%      f *= 1e-17; err *= 1e-17;
      % switch off telluric lines because they are already removed
      set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
      set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
      set_par(sprintf("telluric(1).d%d_barycorr",id),0,1);
    }
    if(spectype[id-1]=="ESPaDOnS"){
      % ESPaDOnS format:
      l = fits_read_img(specs[id-1])[0,*]*10; % conversion from nm to Angstroem
      f = fits_read_img(specs[id-1])[1,*]; % Normalized according to FITS header
      % ------------------------------------------------------------------
      % Get rid of overlapping regions, which are caused by order merging:
      % Divide overlap in two parts and use left part of 'left' spectrum
      % and right part of 'right' spectrum:
      variable maximum = l[0];
      ind = {0};
      i = 1;
      while(i<length(l))
      {
        if(l[i]>maximum)
        {
          list_append(ind, i);
        	maximum = l[i];
    	    i++;
        }
        else % overlap
        {
          while(l[i]<=maximum)
          {
            list_delete(ind, -1);
            maximum = l[ind[-1]];
            i++;
          }
        }
      }
      ind = list_to_array(ind, Integer_Type);
      l = l[ind];
      f = f[ind];
      vmessage("Removing cosmics - check afterwards that emission lines are not clipped/removed");
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=50, verbose=0);
      remove_cosmics(l,f; cosmics=3, range=25, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      (l,f) = optimize_wavegrid(l,f,res_offset[id-1],res_slope[id-1]);
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      % l *= (1.+barycorr/299792.458); % barycentric correction is already applied in ESPaDOnS pipeline
      f /= median(f); % to have flux values at the order of one
      variable dp = max([700,length(l)/10]);
%      snr = snr_curve(l, f, "der_snr"; data_points=dp);
      snr = snr_curve(l, f, "gauss"; data_points=dp);
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
      err = snr.noise;
      % ------------------------------------------------------------------
    }
    if(spectype[id-1]=="SOAR"){
      % SOAR format
      (CRPIX1, CRVAL1, CDELT1, NAXIS1, target) = fits_read_key(specs[id-1],
        "CRPIX1", "CRVAL1", "CD1_1", "NAXIS1", "OBJECT");
      if(target!=NULL) vmessage("%s", target);
      l = CRVAL1 + ([1:NAXIS1:1]-CRPIX1) * CDELT1; f = fits_read_img(specs[id-1]);
      f = f[[0:length(l)-1:1]];
      ind = where(isnan(f)==0);
      if(length(ind)>0){
        l = l[ind]; f = f[ind];
      }
      % removed edges of the spectrum
      ind = where(3640 < l < 9200); l = l[ind]; f = f[ind];
      % resample to a proper wavelength grid, given res
      (l,f) = optimize_wavegrid(l,f,res_offset[id-1],res_slope[id-1]);
      % barycentric correction
      barycorr = fits_read_key(specs[id-1]+"[0]", "VHELIO");
      if(barycorr!=NULL)
      {
        l *= (1.+barycorr/299792.458);
        set_par(sprintf("telluric(1).d%d_barycorr",id),barycorr,0);
      }
      else
        vmessage("Could not read header 'VHELIO' for heliocentric correction.");
      f /= median(f); % to have flux values at the order of one
      variable dp = max([700,length(l)/10]);
%      snr = snr_curve(l, f, "der_snr"; data_points=dp);
      snr = snr_curve(l, f, "gauss"; data_points=dp);
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
      err = snr.noise;
      if(max(l)<5800.){
        set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
        set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
        set_par(sprintf("telluric(1).d%d_barycorr",id),0,1);
      }
    }
    if(spectype[id-1]=="OSIRIS"){
      % PypeIt/GTC OSIRIS pipeline format:
      if(fits_key_exists(specs[id-1]+"[0]", "TARGET"))
        vmessage("%s", fits_read_key(specs[id-1]+"[0]", "TARGET"));
      if(fits_key_exists(specs[id-1]+"[0]", "MJD"))
        mjd = fits_read_key(specs[id-1]+"[0]", "MJD");
      % HDU 1 = science object (SPAT0655); HDU 2 = spurious edge detection - skip it
      temp = fits_read_table(specs[id-1]+"[1]");
      l = temp.opt_wave[*];           % already in Angstroem
      f = temp.opt_counts[*];           % flux-calibrated flux density
      err = temp.opt_counts_sig[*];     % 1-sigma uncertainty on OPT_FLAM
      if(wave_trim[id-1] != NULL){
        ind = where(l>=min(wave_trim[id-1]) and l<max(wave_trim[id-1]));
        l = l[ind];
        f = f[ind];
        err = err[ind];
      }
      __uninitialize(&temp);
    }
    if(spectype[id-1]=="IACOB"){
      % IACOB format (HERMES, FIES):
      (CRPIX1, CRVAL1, CDELT1, NAXIS1, target) = fits_read_key(specs[id-1],
        "CRPIX1", "CRVAL1", "CDELT1", "NAXIS1", "OBJECT");
      vmessage("%s", target);
      % [0,*] is normalized, [1,*] is not normalized
      l = CRVAL1 + ([1:NAXIS1:1]-CRPIX1) * CDELT1; f = fits_read_img(specs[id-1])[1,*];
      ind = where(isnan(f)==0); l = l[ind]; f = f[ind];
      vmessage("Removing cosmics - check afterwards that emission lines are not clipped/removed");
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=50, verbose=0);
      remove_cosmics(l,f; cosmics=3, range=25, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      (l,f) = optimize_wavegrid(l,f,res_offset[id-1],res_slope[id-1]);
      remove_cosmics(l,f; cosmics=3, range=50, verbose=0); remove_cosmics(l,f; cosmics=3, range=25, verbose=0);
      if(fits_read_key(specs[id-1]+"[0]", "INSTRUME")=="HERMES")
        barycorr = fits_read_key(specs[id-1]+"[0]", "BVCOR");
      else if(fits_read_key(specs[id-1]+"[0]", "INSTRUME")=="FIES")
        barycorr = fits_read_key(specs[id-1]+"[0]", "VHELIO");
      l *= (1.+barycorr/299792.458); % barycentric correction
      set_par(sprintf("telluric(1).d%d_barycorr",id),barycorr,0);
      f /= median(f); % to have flux values at the order of one
      variable dp = max([700,length(l)/10]);
%      snr = snr_curve(l, f, "der_snr"; data_points=dp);
      snr = snr_curve(l, f, "gauss"; data_points=dp);
      vmessage("Median signal-to-noise ratio of spectrum %d is %f", id, median(snr.snr));
      err = snr.noise;
    }
    % -------------
    % apply cut
    if(wave_trim[id-1] != NULL){
      ind = where(l>=min(wave_trim[id-1]) and l<max(wave_trim[id-1]));
      l = l[ind];
      f = f[ind];
      err = err[ind];
      if(max(l)<5800.){
        set_par(sprintf("telluric(1).d%d_airmass",id),0,1; min=0, max=3);
        set_par(sprintf("telluric(1).d%d_pwv",id),0,1);
        set_par(sprintf("telluric(1).d%d_barycorr",id),0,1);
      }
    }
    % -------------
    _for i(0, len_comp-1, 1) % loop over components
    {
      variable ind, temp = where(l<spectrum_fit->grid_info.wavelength_grids[i].all[0]*(1+1000/299792.458) or
                                 l>spectrum_fit->grid_info.wavelength_grids[i].all[-1]*(1-1000/299792.458), &ind);
      % factors (1+-1000/299792.458) are buffers to account for unknown radial velocity
      if(length(temp)>0)
      {
	vmessage("\n----------------------------------------------------------------------------------------");
	vmessage("Warning: Observation number %d exceeds wavelength coverage of grid number %d. Trimming it.", id, i+1);
	vmessage("----------------------------------------------------------------------------------------\n");
	l = l[ind]; f = f[ind]; err = err[ind];
      }
    }
    ind = unique(ind); l = l[ind]; f = f[ind]; err = err[ind];
    ind = array_sort(l); l = l[ind]; f = f[ind]; err = err[ind];
    l = 0.5*(l + make_hi_grid(l)); l = [2.*l[0]-l[1],l[[:-2]]]; % conversion to lower bin boundary
    () = define_counts(l, make_hi_grid(l), f, err);
    __uninitialize(&temp);
    variable keys = struct{mjd=-1, fpath=specs[id-1]};
    if(mjd!=NULL) keys.mjd = mjd;
    fits_write_binary_table(sprintf("%sfitsfiles/d%d_spectrum.fits", wd, id), "Spectrum", get_data_counts(id), keys);
    () = system(sprintf("gzip %sfitsfiles/d%d_spectrum.fits", wd, id));
  }
}
%
% flux-calibrated spectra are fitted (see 'initialize_grid_fit_spectroscopy' above)
if(spectrum_fit->stellar_spectrum.nonorm==1)
{
  initialize_theta_interext(; datasets=len_sets);
  fit_fun("theta_interext*"+get_fit_fun);
}
else % Global normalization: fit a cubic spline continuum
{
  l = Array_Type[len_sets];
  f = Array_Type[len_sets];
  _for id(1, len_sets, 1) % loop over datasets
  {
    l[id-1] = union(get_data_counts(id).bin_lo[0]-1, get_data_counts(id).bin_lo[-1]+1, cspline_anchorpoints[id-1]);
    l[id-1] = (l[id-1])[where(get_data_counts(id).bin_lo[0]-1 <= l[id-1] <= get_data_counts(id).bin_lo[-1]+1)];
    f[id-1] = 0*l[id-1] + median(get_data_counts(id).value);
  }
  initialize_cspline(l,f; akima);
  fit_fun("cspline*"+get_fit_fun);
  _for id(1, len_sets, 1) % loop over datasets
    set_par(sprintf("cspline(1).d%d_y*",id); min=0, max=2*max(get_data_counts(id).value));
}
%
_for id(1, len_sets, 1) % loop over datasets
{
  set_par(sprintf("*(1).d%d_res_offset",id), res_offset[id-1], 1);
  set_par(sprintf("*(1).d%d_res_slope",id), res_slope[id-1], 1);
  _for i(1, len_comp, 1) % loop over components
  {
    if(struct_field_exists(spectrum_fit->grid_info.coverage[i-1], "HE3"))
    {
      set_par_fun(sprintf("stellar(1).d%d_c%d_HE",id,i),
                  sprintf("log10(10^stellar(1).d%d_c%d_HE3+10^stellar(1).d%d_c%d_HE4)",id,i,id,i));
    }
  }
}
% link metallicity to iron abundance:
variable Z2FE = 1;
if(Z2FE) link_metallicity_to_iron();;
% --------------------------------
% load prior results if available:
% params:
if(stat_file(sprintf("%sparams/params", wd))!=NULL) load_par(sprintf("%sparams/params", wd));;
% ignore lists:
_for id(1, len_sets, 1) % loop over datasets
{
  if(get_par(sprintf("stellar(1).d%d_res_offset",id))==0 &
     get_par(sprintf("stellar(1).d%d_res_slope",id))==0)
  {
    vmessage("ERROR: res_offset = res_slope = 0 is not physical. Set correct R.");
    exit;
  }
  if(stat_file(sprintf("%slists/d%d_ignore_list.txt", wd, id))==NULL)
  {
    variable fpt = fopen(sprintf("%slists/d%d_ignore_list.txt", wd, id), "w");
%   () = fprintf(fpt, "%.3f  %.3f  %% miscellaneous\n", 1000, 3850);
    () = fprintf(fpt, "%.3f  %.3f  %% CaII IS\n", 3933.663*(1.-100/299792.458), 3933.663*(1.+100/299792.458));
    () = fprintf(fpt, "%.3f  %.3f  %% CaII IS\n", 3968.469*(1.-100/299792.458), 3968.469*(1.+100/299792.458));
    () = fprintf(fpt, "%.3f  %.3f  %% NaI IS\n",  5889.950*(1.-100/299792.458), 5889.950*(1.+100/299792.458));
    () = fprintf(fpt, "%.3f  %.3f  %% NaI IS\n",  5895.920*(1.-100/299792.458), 5895.920*(1.+100/299792.458));
    () = fclose(fpt);
    __uninitialize(&fpt);
  }
  variable col1, col2;
  (col1,col2) = readcol(sprintf("%slists/d%d_ignore_list.txt",wd,id), 1, 2);
  _for i(0, length(col1)-1, 1)
    ignore(id, col1[i], col2[i]);
}

% linelists:
variable ll_default = ascii_read_table(ll_default_path, [{"%s","element"}, {"%s", "ion_level"},
                                                         {"%F", "wavelength"}, {"%s", "reliability"}]);

define process_ll_default(ll_default)
{
  variable ll = {};
  _for id(1, len_sets, 1) % loop over datasets
  {
    temp = Struct_Type[len_comp];
    _for i(1, len_comp, 1) % loop over components
    {
      if(stat_file(sprintf("%slists/d%d_c%d_linelist.fits", wd, id, i))!=NULL)
      {
        temp[i-1] = fits_read_table(sprintf("%slists/d%d_c%d_linelist.fits", wd, id, i));
      }
      else
      {
        if(struct_field_exists(spectrum_fit->grid_info.coverage[i-1], "HE3"))
          temp[i-1] = ll_default;
        else
          temp[i-1] = struct_filter(ll_default, where(ll_default.element!="He3"); copy);
      }
      % only HHE
      if(length(spectrum_fit->grid_info.species[0])==1)
      {
        variable mask_metal = where(not ismember(temp[i-1].element, ["H", "He"]));
        temp[i-1].reliability[mask_metal] = "absent";
      }
    }
    list_append(ll, temp);
    __uninitialize(&temp);
  }
  return ll;
}

variable ll = process_ll_default(ll_default);

% --------------------------------
%}}}

% set_fit_method("mpfit;ftol=1e-6;xtol=1e-6;gtol=1e-6");
set_fit_method("powell");
() = eval_counts();

save_par("params/params");

#ifeval 0

% First guess from photometry:
% ============================
variable photo = photometric_table("");
photo.read("../photometry/photometry.dat");
photo.photometric_entries.uncertainty[where(photo.photometric_entries.flag==0 and (photo.photometric_entries.uncertainty<1e-5 or isnan(photo.photometric_entries.uncertainty)==1))] = 0.025;
set_fit_method("mpfit");
initialize_grid_fit_photometry(griddirectories);
variable p = photometric_fitting(photo.photometric_entries; fit_verbose=-1, verbose=1, set_par=struct{name=["R_55","c*_xi","c*_z","c*_HE"],value=[3.02,0,0,-1.05],freeze=[1,1,1,1]}, conf_level=-1, norm_chi_red, remove_outliers=5);
_for i(1, len_comp, 1) {set_par(sprintf("stellar(1).d*_c%d_teff",i), p.value[where(p.name==sprintf("c%d_teff",i))[0]]); set_par(sprintf("stellar(1).d*_c%d_logg",i), p.value[where(p.name==sprintf("c%d_logg",i))[0]]);};
set_fit_method("powell");

% First guess for radial velocity:
% ================================
_for id(1, len_sets, 1) % loop over datasets
{
  variable out = fit_vrad(get_data_counts(id).bin_lo, get_data_counts(id).value, struct{ wavelength = [4128.067, 4567.84, 5015.678, 6678.151], range_left = [2, 5, 1, 5], range_right = [2, 5, 2, 5] }; res_plot);
  set_par(sprintf("stellar(1).d%d_c*vrad",id), median(out.vrad[where(isnan(out.vrad)==0)])); () = eval_counts;
};

% Tie fit-parameters of all datasets to those of the first dataset:
% ================================================================= %{{{
_for i(1, len_comp, 1) % loop over components
{
  variable field;
  foreach field(get_struct_field_names(spectrum_fit->params.macro_broad[i-1])) tie(sprintf("stellar(1).d1_c%d_",i)+field, sprintf("stellar(1).d*_c%d_",i)+field);;
  % tie(sprintf("stellar(1).d1_c%d_vrad",i), sprintf("stellar(1).d*_c%d_vrad",i));
  tie(sprintf("stellar(1).d1_c%d_teff",i), sprintf("stellar(1).d*_c%d_teff",i));
  tie(sprintf("stellar(1).d1_c%d_logg",i), sprintf("stellar(1).d*_c%d_logg",i));
  tie(sprintf("stellar(1).d1_c%d_xi",i), sprintf("stellar(1).d*_c%d_xi",i));
  tie(sprintf("stellar(1).d1_c%d_HE",i), sprintf("stellar(1).d*_c%d_HE",i));
  foreach field(spectrum_fit->grid_info.species[i-1]) tie(sprintf("stellar(1).d1_c%d_",i)+field, sprintf("stellar(1).d*_c%d_",i)+field);;
  tie(sprintf("stellar(1).d1_c%d_sur_ratio",i), sprintf("stellar(1).d*_c%d_sur_ratio",i));
};%}}}

% First guess for continuum:
% ==========================
variable temp = get_params("stellar(1).*"); freeze("stellar(1).*");
variable free = freeParameters; freeze(free); exclude(all_data);
_for id(1, len_sets, 1) % loop over datasets
{
  include(id);
  thaw(free[(where(array_map(Integer_Type, &string_match, array_map(String_Type, &get_struct_field, get_params(free), "name"), sprintf(".d%d_",id))!=0))]); % thaw previously free parameters containing the string ".d$id_" in their name
  set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
  exclude(id); freeze("*");
};
include(all_data); thaw(free); set_params(temp); __uninitialize(&temp);

% Some commands:
% ==============
hotkeys(; linelist=ll);
set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
save_par("params/params"); save_par("params/params_1");

% Specific linelist:
% ==================
% restart script to load default linelist as input for 'create_specific_linelist'
() = system("rm -f lists/d*linelist.fits"); () = evalfile("./"+__argv[1], current_namespace);
_for id(0, len_sets-1, 1) ll[id] = create_specific_linelist(id+1, ll[id]; save="lists/", absent_chi_threshold=1);;

% Apply spectral mask to ignore regions:
% ======================================
_for id(0, len_sets-1, 1) % loop over datasets
{
  variable mask = Struct_Type[len_comp];
  _for i(0, len_comp-1, 1) % loop over components
  {
    ind = where(ll[id][i].reliability=="absent" and ll[id][i].ion_level!="IS"); % add absent stellar lines in the given linelist to the spectral mask
    if(get_par(sprintf("stellar(1).d%d_c%d_teff",id+1,i+1))>12000) % also add the cores of the Balmer lines and helium lines with forbidden components to the spectral mask if the effective temperature is larger than 12000 K
      ind = union(ind, where((ll[id][i].element=="H" and ll[id][i].wavelength<7000) or (ll[id][i].element=="He" and ll[id][i].reliability=="bad")));
    mask[i] = struct_filter(ll[id][i], ind; copy);
    struct_filter(mask[i], array_sort(mask[i].wavelength)); % sort the lines in the mask in ascending order
  }
  create_ignore_list_from_spectral_mask(id+1, mask; ignore_list=sprintf("%slists/d%d_ignore_list.txt", wd, id+1));;
};
set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
save_par("params/params"); save_par("params/params_1");

% Freeze continuum points in ignored regions:
% ===========================================
if(string_match(get_fit_fun, "cspline")!=0)
{
  variable temp1 = get_params("stellar(1).*"); freeze("stellar(1).*");
  variable free = freeParameters; freeze(free); exclude(all_data);
  _for id(1, len_sets, 1) % loop over datasets
  {
    variable temp2 = get_data_info(id).notice_list;
    include(id); notice(id);
    % thaw previously free parameters containing the string ".d$id_" in their name
    thaw(free[(where(array_map(Integer_Type, &string_match, array_map(String_Type, &get_struct_field, get_params(free), "name"), sprintf(".d%d_",id))!=0))]);
    set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
    ignore(id); notice_list(id, temp2); () = eval_counts(; fit_verbose=-1);
    variable temp3 = length(get_par(sprintf("cspline(1).d%d_x*",id)));
    % freeze continuum point 'i' when no noticed pixels are between anchorpoints 'i-1' and 'i+1'
    _for i(0, temp3-1, 1)
    {
      variable temp4 = get_par(sprintf("cspline(1).d%d_x%d",id,max([0,i-1])));
      variable temp5 = get_par(sprintf("cspline(1).d%d_x%d",id,min([i+1,temp3-1])));
      % no noticed pixels between anchorpoints 'i-1' and 'i+1'
      if(length(where(temp4<get_data_counts(id).bin_lo[temp2]<temp5))==0)
      {
        variable temp6;
        foreach temp6 (["x","y"])
        {
          variable temp7 = get_par_info(sprintf("cspline(1).d%d_%s%d",id,temp6,i));
          freeze(temp7.index);
          free = free[where(free!=temp7.index)];
          __uninitialize(&temp7);
        }
        __uninitialize(&temp6);
      }
      __uninitialize(&temp4); __uninitialize(&temp5);
    }
    __uninitialize(&temp2); __uninitialize(&temp3);
    set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
    exclude(id); freeze("*");
  }
  include(all_data); thaw(free); set_params(temp1); __uninitialize(&temp1);
  save_par("params/params"); save_par("params/params_1");
};

% Removal of telluric lines:
% ==========================
if(string_match(get_fit_fun, "telluric")!=0)
{
  variable temp = get_params("stellar*"); freeze("stellar*");
  variable free = freeParameters; freeze(free); exclude(all_data);
  variable conf_ind = Integer_Type[0], conf_min = Double_Type[0], conf_max = Double_Type[0];
  _for id(1, len_sets, 1) % loop over datasets
  {
    include(id);
    thaw(free[(where(array_map(Integer_Type, &string_match, array_map(String_Type, &get_struct_field, get_params(free), "name"), sprintf(".d%d_",id))!=0))]); % thaw previously free parameters containing the string ".d$id_" in their name
    () = fit_counts;
    variable temp1, temp2, temp3 = freeParameters(; fit_fun_component="telluric");
    if(length(temp3)>0)
    {
      (temp1, temp2) = conf_loop(temp3, 0, 1e-1; num_slaves=1, max_num_retries=100);
      conf_min = [conf_min, temp1]; conf_max = [conf_max, temp2]; conf_ind = [conf_ind, temp3];
      __uninitialize(&temp1); __uninitialize(&temp2); __uninitialize(&temp3);
    }
    exclude(id); freeze("*");
  }
  temp1 = where(conf_min==conf_max);
  % when 'conf_loop' encounters more than 'max_num_retries'-times an improved fit,
  % it will return 0 for conf_min and conf_max
  conf_min[temp1] = get_par(conf_ind[temp1]); conf_max[temp1] = get_par(conf_ind[temp1]);
  fits_write_binary_table("fitsfiles/results_telluric_conf.fits", "pvm_fit_pars-results",
                          conf_loop_summary(conf_ind, conf_min, conf_max));
  include(all_data); thaw(free); set_params(temp); __uninitialize(&temp); __uninitialize(&temp1);
  save_par("params/params"); save_par("params/params_1");
  _for id(1, len_sets, 1) % loop over datasets
  {
    variable temp = get_data_counts(id);
    temp.value /= spectrum_fit->telluric_spectrum[id-1];
    temp.err /= spectrum_fit->telluric_spectrum[id-1]; % to conserve the chi^2 statistics
    put_data(id, temp); % replace dataset
    __uninitialize(&temp);
    fits_write_binary_table(sprintf("%sfitsfiles/d%d_spectrum.fits", wd, id), "Spectrum",
			    struct_combine(fits_read_table(sprintf("%sfitsfiles/d%d_spectrum.fits.gz", wd, id)),
                               struct{telluric_spectrum=spectrum_fit->telluric_spectrum[id-1]},
                               get_data_counts(id)));
    () = system(sprintf("gzip -f %sfitsfiles/d%d_spectrum.fits", wd, id));
  }
  fit_fun(strreplace(get_fit_fun,"*telluric","")); () = eval_counts;
  save_par("params/params"); save_par("params/params_2");
};

% Local normalization: (useful for detailed abundance studies based on high-resolution spectra)
% ====================
variable cc = continuum_correction(ll; interactive);
_for id(1, len_sets, 1) % loop over datasets
{
  variable temp = get_data_counts(id);
  variable old = fits_read_table(sprintf("%sfitsfiles/d%d_spectrum.fits.gz", wd, id));
  if(struct_field_exists(old,"continuum_correction")) % combine old and new continuum correction
  {
    cc[id-1] *= old.continuum_correction;
    temp.value *= old.continuum_correction;
    temp.err *= mean(old.continuum_correction);
  }
  temp.value /= cc[id-1];
  temp.err /= mean(cc[id-1]); % to roughly conserve the chi^2 statistics
  put_data(id, temp); % replace dataset
  __uninitialize(&temp);
  fits_write_binary_table(sprintf("%sfitsfiles/d%d_spectrum.fits", wd, id), "Spectrum",
                          struct_combine(old, struct{continuum_correction=cc[id-1]}, get_data_counts(id)));
  () = system(sprintf("gzip -f %sfitsfiles/d%d_spectrum.fits", wd, id));
};
__uninitialize(&cc);
set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
save_par("params/params"); save_par("params/params_3");

% Line analysis (should be used before adding systematics, e.g., at the best fit with local normalization):
% =============
load_par("params/params_3"); () = eval_counts;
variable oa, dla;
_for id(1, len_sets, 1) % loop over datasets
  (oa, dla) = line_analysis(id, ll[id-1]; save="lists/");;

% Systematics (start from the best fit with local normalization):
% ===========
load_par("params/params_3"); () = eval_counts;
add_systematic_errors(; save="fitsfiles/");
() = system(sprintf("gzip -f %sfitsfiles/d*_spectrum.fits", wd));
set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
save_par("params/params"); save_par("params/params_4");

% Statistical errors (after systematics):
% ==================
stellar_set_ranges(5, 0.5, 100, 0.02, 0.5, 0.2, 0.05, 0.15);
if(Z2FE) link_metallicity_to_iron();
if(string_match(get_fit_fun, "cspline")!=0){freeze("cspline(1).*");};
save_par("params/params");
% _for i(1, len_comp, 1) % loop over components
% {
%   set_par(sprintf("stellar(1).d*_c%d_AR",i); min=spectrum_fit->grid_info.coverage[i-1].AR[0], max=spectrum_fit->grid_info.coverage[i-1].AR[-1]); save_par("params/params");
%   temp = get_par_info(sprintf("stellar(1).d*_c%d_m*_k0",i));
%   if(temp!=NULL)
%     _for j(0, length(temp)-1, 1) set_par(temp[j].name; min=_max(0,temp[j].value-0.1), max=temp[j].value+0.1); save_par("params/params");;
%   __uninitialize(&temp);
% };
% Option 1: computation on the Remeis cluster:
mpi_fit_pars_errors(wd, __argv[1]; level=0, fit_fun_component="stellar");
() = system("sbatch mpi_fit_pars.slurm");
load_par("mpi_fit_pars_results_best.par"); save_par("params/params"); save_par("params/params_5");
() = system("mv mpi_fit_pars_results_conf.fits fitsfiles/results_conf.fits");
() = system("rm mpi_fit_pars*");
% Option 2: computation on a single computer:
(conf_min, conf_max) = conf_loop(freeParameters(; fit_fun_component="stellar"), 0, 1e-1; num_slaves=1);
save_par("params/params"); save_par("params/params_5");
variable stat; () = eval_counts(&stat; fit_verbose=-1);
fits_write_binary_table("fitsfiles/results_conf.fits", "pvm_fit_pars-results", conf_loop_summary(freeParameters(; fit_fun_component="stellar"), conf_min, conf_max), struct{
  chi2 = stat.statistic, num_bins = stat.num_bins, n_var_pars = stat.num_variable_params, dof = stat.num_bins-stat.num_variable_params,
  chi2red = stat.statistic/(stat.num_bins-stat.num_variable_params)
} );
% % Edit entries by hand:
% temp = fits_read_table("fitsfiles/results_conf.fits");
% temp.conf_max[where(temp.name=="stellar(1).d1_c1_xi")] = _max(temp.conf_max[where(temp.name=="stellar(1).d1_c1_xi")],3);
% variable stat; () = eval_counts(&stat; fit_verbose=-1);
% fits_write_binary_table("fitsfiles/results_conf.fits", "pvm_fit_pars-results", conf_loop_summary(freeParameters(; fit_fun_component="stellar"), temp.conf_min, temp.conf_max), struct{
%     chi2 = stat.statistic, num_bins = stat.num_bins, n_var_pars = stat.num_variable_params, dof = stat.num_bins-stat.num_variable_params,
%     chi2red = stat.statistic/(stat.num_bins-stat.num_variable_params)
% } );

% Systematic errors, confidence maps, and stellar parameters (after statistical errors):
% ==========================================================
load_par("params/params_5"); stellar_set_ranges(); if(Z2FE) link_metallicity_to_iron();; save_par("params/params"); save_par("params/params_5"); () = eval_counts; % increase parameter ranges
variable s = fits_read_table("fitsfiles/results_conf.fits");
_for id(1, len_sets, 1) % loop over datasets
{
  % -----------------------------------------------------------------------------------------------------------------------------------------------------------------
  % Run 'photometric_fitting' with qualifier 'remove_outliers' to flag outliers in photo.photometric_entries, which is used by the function 'stellar_params_confmap':
  variable par = struct{ name=["R_55"], value=[3.02], freeze=[1] };
  variable photo = photometric_table("");
  photo.read("../photometry/photometry.dat"); % read the table anew to get rid of previous outlier flags
  photo.photometric_entries.uncertainty[where(photo.photometric_entries.flag==0 and (photo.photometric_entries.uncertainty<1e-5 or isnan(photo.photometric_entries.uncertainty)==1))] = 0.025;
  variable temp = Struct_Type[len_comp]; _for i(0, len_comp-1, 1){ temp[i] = struct{metal=["ATLAS12"]}; };
  variable atl = struct{l,f};
  (atl.l, atl.f) = syn_spec(get_par(sprintf("stellar(1).d%d_c*_teff",id)), get_par(sprintf("stellar(1).d%d_c*_logg",id)), get_par(sprintf("stellar(1).d%d_c*_xi",id)), get_par(sprintf("stellar(1).d%d_c*_z",id)),
			    get_par(sprintf("stellar(1).d%d_c*_HE",id)), spectrum_fit->grid_info; vrad=get_par(sprintf("stellar(1).d%d_c*_vrad",id)), sur_ratio=get_par(sprintf("stellar(1).d%d_c*_sur_ratio",id)),
			    metals=temp);
  () = photometric_fitting(atl.l, atl.f, photo.photometric_entries; fit_verbose=-1, verbose=1, set_par=par, conf_level=0, norm_chi_red, remove_outliers=5);
  __uninitialize(&temp); __uninitialize(&atl);
  % -----------------------------------------------------------------------------------------------------------------------------------------------------------------
  _for i(1, len_comp, 1) % loop over components
  {
    variable xpar = sprintf("stellar(1).d%d_c%d_teff",id,i);
    variable ypar = sprintf("stellar(1).d%d_c%d_logg",id,i);
    if(get_par_info(xpar).fun==NULL && get_par_info(xpar).tie==NULL && get_par_info(ypar).fun==NULL && get_par_info(ypar).tie==NULL)
    {
      variable xpar_i = where(s.name==xpar)[0];
      variable ypar_i = where(s.name==ypar)[0];
      variable xpar_r = 0.01*s.value[xpar_i]; % 1 per cent systematic uncertainty
      variable ypar_r = 0.04; % 0.04 dex systematic uncertainty
      variable xpar_n = 9;
      variable ypar_n = 9;
      variable xpar_lo = s.value[xpar_i] - sqrt((s.value[xpar_i]-s.conf_min[xpar_i])^2+xpar_r^2);
      variable xpar_hi = s.value[xpar_i] + sqrt((s.conf_max[xpar_i]-s.value[xpar_i])^2+xpar_r^2);
      variable ypar_lo = s.value[ypar_i] - sqrt((s.value[ypar_i]-s.conf_min[ypar_i])^2+ypar_r^2);
      variable ypar_hi = s.value[ypar_i] + sqrt((s.conf_max[ypar_i]-s.value[ypar_i])^2+ypar_r^2);
      % make sure that the spot of the best fit is sampled in confmap:
      (xpar_lo, xpar_hi, xpar_n) = conf_grid_bestfit(xpar_lo, xpar_hi, xpar_n, s.value[xpar_i]);
      (ypar_lo, ypar_hi, ypar_n) = conf_grid_bestfit(ypar_lo, ypar_hi, ypar_n, s.value[ypar_i]);
      % confidence maps:
      variable out = sprintf("d%d_c%d_%s_%s",id,i,strreplace(xpar,sprintf("stellar(1).d%d_c%d_",id,i),""),
                                                  strreplace(ypar,sprintf("stellar(1).d%d_c%d_",id,i),""));
      variable temp = get_params("stellar(1).*"); save_pars = Integer_Type[length(temp)];
      _for j(0,length(temp)-1,1){save_pars[j]=temp[j].index;}; __uninitialize(&temp);
      % compute confidence map
      variable confmap = get_confmap(xpar, xpar_lo, xpar_hi, xpar_n, ypar, ypar_lo, ypar_hi, ypar_n;
                         save=sprintf("fitsfiles/confmap_%s",out), flood, num_slaves=1, save_pars=save_pars);
      () = system(sprintf("gzip -f %sfitsfiles/confmap_%s.fits", wd, out));
      variable r = confmap_errors(sprintf("fitsfiles/confmap_%s.fits.gz",out), xpar_lo, xpar_hi, ypar_lo, ypar_hi; stat_sys="fitsfiles/results_conf.fits", xsys=xpar_r, ysys=ypar_r);
      variable temp = get_par_info("stellar(1).*"); r = struct_combine(r,struct{range_min=array_map(Double_Type,&get_struct_field,temp,"min"), range_max=array_map(Double_Type,&get_struct_field,temp,"max")}); __uninitialize(&temp);
      print_struct(sprintf("results/atmos_params_%s.txt",out), r);
      xfig_plot_teff_logg_confmap(sprintf("fitsfiles/confmap_%s.fits.gz",out), xpar_lo, xpar_hi, ypar_lo, ypar_hi);
      % mass fractions:
      mass_fraction_confmap(sprintf("fitsfiles/confmap_%s.fits.gz",out), spectrum_fit->grid_info);
      () = system(sprintf("gzip -f %sfitsfiles/confmap_%s_mass_fractions.fits", wd, out));
      variable h, k, temp = struct{ name=String_Type[0], value=Double_Type[0], conf_min=Double_Type[0], conf_max=Double_Type[0] };
      _for h(1, len_sets, 1)
      {
        _for k(1, len_comp, 1)
        {
          variable abundances = ascii_read_table(spectrum_fit->grid_info.location[k-1]+"abundances.dat", [{"%I", "atomic_number"}, {"%s", "element"}, {"%F", "log_number_fraction"}]);
          abundances.log_number_fraction[[1:]] += get_par(sprintf("stellar(1).d%d_c%d_z",h,k)); % scale metal abundances according to the fit-parameter 'z'
          variable species = @spectrum_fit->grid_info.species[k-1]; species = species[where(species!="HHE")];
          if(get_par_info(sprintf("stellar(1).d%d_c%d_HE3",h,k))!=NULL)
          {
            abundances.atomic_number = [2,2,abundances.atomic_number]; abundances.element = ["HE3","HE4",abundances.element]; abundances.log_number_fraction = [-4.89,-1.05,abundances.log_number_fraction];
            species = ["HE3","HE4",species];
          }
          else
          {
            abundances.atomic_number = [2,abundances.atomic_number]; abundances.element = ["HE4",abundances.element]; abundances.log_number_fraction = [-1.05,abundances.log_number_fraction];
            species = ["HE4",species];
            s.name[where(s.name==sprintf("stellar(1).d%d_c%d_HE",h,k))] = sprintf("stellar(1).d%d_c%d_HE4",h,k);
          }
          abundances = struct_combine(abundances, "log_number_fraction_min", "log_number_fraction_max");
          abundances.log_number_fraction_min = @abundances.log_number_fraction; abundances.log_number_fraction_max = @abundances.log_number_fraction;
          variable j, temp1, temp2, temp3={}, len = length(species);
          _for j(0, len-1, 1)
          {
            temp1 = where(abundances.element==species[j]);
	    if(length(temp1)==1)
	    {
	      temp2 = where(s.name==sprintf("stellar(1).d%d_c%d_%s",h,k,species[j]));
	      if(length(temp2)==1)
	      {
		abundances.log_number_fraction[temp1[0]] = s.value[temp2[0]]; abundances.log_number_fraction_min[temp1[0]] = s.conf_min[temp2[0]]; abundances.log_number_fraction_max[temp1[0]] = s.conf_max[temp2[0]];
		list_append(temp3, species[j]);
	      }
	      else
	      {
		if(get_par_info(sprintf("stellar(1).d%d_c%d_HE3",h,k))==NULL and species[j]=="HE4")
		  species[j]="HE";
		abundances.log_number_fraction[temp1[0]] = get_par(sprintf("stellar(1).d%d_c%d_%s",h,k,species[j]));
		abundances.log_number_fraction_min[temp1[0]] = get_par(sprintf("stellar(1).d%d_c%d_%s",h,k,species[j]));
		abundances.log_number_fraction_max[temp1[0]] = get_par(sprintf("stellar(1).d%d_c%d_%s",h,k,species[j]));
	      }
	    }
	    __uninitialize(&temp1); __uninitialize(&temp2);
	  }
	  (, , , abundances) = mass_fraction(; abundances=abundances);
          species = list_to_array(temp3, String_Type); __uninitialize(&temp3); len = length(species);
          temp1 = struct{ name=String_Type[len], value=Double_Type[len], conf_min=Double_Type[len], conf_max=Double_Type[len] };
          _for j(0, len-1, 1)
          {
            temp1.name[j] = sprintf("stellar(1).d%d_c%d_%s",h,i,species[j]);
            temp2 = where(abundances.element==species[j])[0];
            temp1.value[j] = abundances.log_mass_fraction[temp2];
            temp1.conf_min[j] = abundances.log_mass_fraction_min[temp2];
            temp1.conf_max[j] = abundances.log_mass_fraction_max[temp2];
            __uninitialize(&temp2);
          }
          temp = merge_struct_arrays([temp, temp1]);
          __uninitialize(&temp1);
        }
      }
      fits_write_binary_table("fitsfiles/mass_fractions_conf.fits", NULL, temp); __uninitialize(&temp);
      variable mf = confmap_errors(sprintf("%sfitsfiles/confmap_%s_mass_fractions.fits.gz", wd, out), xpar_lo, xpar_hi, ypar_lo, ypar_hi; stat_sys="fitsfiles/mass_fractions_conf.fits");
      print_struct(sprintf("results/mass_fractions_%s.txt",out), mf);
      % stellar parameters:
      stellar_params_confmap(sprintf("fitsfiles/confmap_%s.fits.gz",out), photo.photometric_entries, spectrum_fit->grid_info; fit_verbose=-1, set_par=par, conf_level=0, norm_chi_red, grid="georgy", points=[[1:85:1],[130:189:1]]);
      () = system(sprintf("gzip -f %sfitsfiles/confmap_%s_stellar_params.fits", wd, out));
      % variable cm = fits_read_table(sprintf("%sfitsfiles/confmap_%s_stellar_params.fits.gz", wd, out));
      % variable sp = confmap_errors(sprintf("%sfitsfiles/confmap_%s_stellar_params.fits.gz", wd, out), xpar_lo, xpar_hi, ypar_lo, ypar_hi; exclude=where(cm.c1_chisqr_evol_track>10));
      variable sp = confmap_errors(sprintf("%sfitsfiles/confmap_%s_stellar_params.fits.gz", wd, out), xpar_lo, xpar_hi, ypar_lo, ypar_hi);
      print_struct(sprintf("results/stellar_params_%s.txt",out), sp);
    }
  }
};

% Quick-look SED (spectral energy distribution):
% ============== %{{{
load_par("params/params_5"); () = eval_counts;
variable temp = Struct_Type[len_comp]; _for i(0, len_comp-1, 1){ temp[i] = struct{metal=["ATLAS12"]}; };
_for id(1, len_sets, 1) % loop over datasets
{
  variable atl = struct{l,f};
  (atl.l, atl.f) = syn_spec(get_par(sprintf("stellar(1).d%d_c*_teff",id)), get_par(sprintf("stellar(1).d%d_c*_logg",id)), get_par(sprintf("stellar(1).d%d_c*_xi",id)), get_par(sprintf("stellar(1).d%d_c*_z",id)),
			    get_par(sprintf("stellar(1).d%d_c*_HE",id)), spectrum_fit->grid_info; vrad=get_par(sprintf("stellar(1).d%d_c*_vrad",id)), sur_ratio=get_par(sprintf("stellar(1).d%d_c*_sur_ratio",id)),
			    metals=temp);
  variable norm_chi_red = 0;
  variable p = photometric_fitting(atl.l, atl.f, photo.photometric_entries; fit_verbose=-1, verbose=1, set_par=struct{ name=["R_55"], value=[3.02], freeze=[1] }, conf_level=-1, norm_chi_red=&norm_chi_red, remove_outliers=5);
  xfig_photometry(atl.l, atl.f, photo.photometric_entries; xmin=1000, xmax=199999, colored, errbar, filter_width, chi,
		  theta=10^p.value[where(p.name=="logtheta")[0]], E_44m55=p.value[where(p.name=="E_44m55")[0]], R_55=p.value[where(p.name=="R_55")[0]]).render(sprintf("d%d_SED.pdf",id));
};
%}}}

% Collect results:
% ================
variable files = struct{ atmos=glob("results/atmos*"), mass_fractions=glob("results/mass_fractions*"), stellar=glob("results/stellar*") };
variable s = spectroscopy_collect_results("", files);
% Print TeX tables:
variable atmos = spectroscopy_TeX_tables_atmos(s.atmos);
atmos.object = array_map(String_Type, &strreplace, atmos.object, "_", "\\_");
% atmos.sur_ratio = array_map(String_Type, &sprintf, "   ", atmos.sur_ratio);
% atmos.sur_ratio_stat = array_map(String_Type, &sprintf, "   ", atmos.sur_ratio_stat);
% atmos.sur_ratio_sys = array_map(String_Type, &sprintf, "   ", atmos.sur_ratio_sys);
variable filename = "table_atmospheric_params";
variable fp = fopen(sprintf("results/%s.tex",filename),"w");
() = fprintf(fp, "\documentclass{standalone}"R+"\n"+"\usepackage{amsmath,txfonts,color}"R+"\n"+"\begin{document}"R+"\n"+"\renewcommand{\arraystretch}{1.1}"R+"\n");
() = fprintf(fp, "\begin{tabular}{l"R + multiple_string(length(get_struct_field_names(atmos))/3-1, "r") + "}\n"+"\hline\hline"R+"\n");
() = fprintf(fp, "Object & $T_{\mathrm{eff}}$ & $\log(g)$ & $\varv_{\mathrm{rad}}$ & $\varv\sin(i)$ & $\zeta$ & $\xi$ & $A_\mathrm{eff}/A_{\mathrm{eff,}1}$ & \multicolumn{11}{c}{$\log(n(x))$} \\"R+"\n"+"\cline{4-7} \cline{9-19}"R+"\n"+
             "& (K) & (cgs) & \multicolumn{4}{c}{(km\,s$^{-1}$)} & & He & C & N & O & Ne & Mg & Al & Si & S & Ar & Fe\\"R+"\n"+"\hline"R+"\n");
variable fields = ["teff", "logg", "vrad", "vsini", "zeta", "xi", "sur_ratio", "HE", "C", "N", "O", "NE", "MG", "AL", "SI", "S", "AR", "FE"];
_for i(0, length(atmos.object)-1, 1)
{
  print_struct(fp, atmos; sep=" & ", nohead, fields=["object", fields], i=[i]);
  print_struct(fp, atmos; sep=" & ", nohead, fields=["stat", fields+"_stat"], i=[i]);
  print_struct(fp, atmos; sep=" & ", final=" \\"R, nohead, fields=["sys", fields+"_sys"], i=[i]);
}
() = fprintf(fp, "\hline"R+"\n"+"\end{tabular}"R+"\n"+"\end{document}"R+"\n");
() = fclose(fp);
() = system(sprintf("cd results; pdflatex -halt-on-error -file-line-error %s.tex | grep 'error' --color=always; rm %s.aux %s.log; cd ../", filename, filename, filename));
%
variable massf = spectroscopy_TeX_tables_mass_fractions(s.mass_fractions);
massf.object = array_map(String_Type, &strreplace, massf.object, "_", "\\_");
massf = struct_combine(struct{HE, HE_stat, HE_sys}, massf); massf.HE = massf.HE4; massf.HE_stat = massf.HE4_stat; massf.HE_sys = massf.HE4_sys; massf = reduce_struct(massf, ["HE4","HE4_stat","HE4_sys"]); % convert HE4 to HE
atmos = struct_combine(atmos, massf); % overwrite number fractions by mass fractions in structure "atmos"
% atmos.sur_ratio = array_map(String_Type, &sprintf, "   ", atmos.sur_ratio);
% atmos.sur_ratio_stat = array_map(String_Type, &sprintf, "   ", atmos.sur_ratio_stat);
% atmos.sur_ratio_sys = array_map(String_Type, &sprintf, "   ", atmos.sur_ratio_sys);
variable filename = "table_atmospheric_params_with_mass_fractions";
variable fp = fopen(sprintf("results/%s.tex",filename),"w");
() = fprintf(fp, "\documentclass{standalone}"R+"\n"+"\usepackage{amsmath,txfonts,color}"R+"\n"+"\begin{document}"R+"\n"+"\renewcommand{\arraystretch}{1.1}"R+"\n");
() = fprintf(fp, "\begin{tabular}{l"R + multiple_string(length(get_struct_field_names(atmos))/3-1, "r") + "}\n"+"\hline\hline"R+"\n");
() = fprintf(fp, "Object & $T_{\mathrm{eff}}$ & $\log(g)$ & $\varv_{\mathrm{rad}}$ & $\varv\sin(i)$ & $\zeta$ & $\xi$ & $A_\mathrm{eff}/A_{\mathrm{eff,}1}$ & \multicolumn{11}{c}{$\log(x)$} \\"R+"\n"+"\cline{4-7} \cline{9-19}"R+"\n"+
             "& (K) & (cgs) & \multicolumn{4}{c}{(km\,s$^{-1}$)} & & He & C & N & O & Ne & Mg & Al & Si & S & Ar & Fe\\"R+"\n"+"\hline"R+"\n");
variable fields = ["teff", "logg", "vrad", "vsini", "zeta", "xi", "sur_ratio", "HE", "C", "N", "O", "NE", "MG", "AL", "SI", "S", "AR", "FE"];
_for i(0, length(atmos.object)-1, 1)
{
  print_struct(fp, atmos; sep=" & ", nohead, fields=["object", fields], i=[i]);
  print_struct(fp, atmos; sep=" & ", nohead, fields=["stat", fields+"_stat"], i=[i]);
  print_struct(fp, atmos; sep=" & ", final=" \\"R, nohead, fields=["sys", fields+"_sys"], i=[i]);
}
() = fprintf(fp, "\hline"R+"\n"+"\end{tabular}"R+"\n"+"\end{document}"R+"\n");
() = fclose(fp);
() = system(sprintf("cd results; pdflatex -halt-on-error -file-line-error %s.tex | grep 'error' --color=always; rm %s.aux %s.log; cd ../", filename, filename, filename));
%
variable stell = spectroscopy_TeX_tables_stellar(s.stellar);
stell.object = array_map(String_Type, &strreplace, stell.object, "_", "\\_");
variable filename = "table_stellar_params";
variable fp = fopen(sprintf("results/%s.tex",filename),"w");
() = fprintf(fp, "\documentclass{standalone}"R+"\n"+"\usepackage{amsmath,txfonts,color}"R+"\n"+"\begin{document}"R+"\n"+"\renewcommand{\arraystretch}{1.1}"R+"\n");
() = fprintf(fp, "\begin{tabular}{l"R + multiple_string(length(get_struct_field_names(stell))-1, "r") + "}\n"+"\hline\hline"R+"\n");
() = fprintf(fp, "Object & & \multicolumn{2}{c}{$X$} & & \multicolumn{2}{c}{$Y$} & & \multicolumn{2}{c}{$Z$} & & \multicolumn{2}{c}{$M$} & & \multicolumn{2}{c}{$\tau$} & & \multicolumn{2}{c}{$\log(L/L_{\odot})$}"R);
() = fprintf(fp, " & & \multicolumn{2}{c}{$R$} & & \multicolumn{2}{c}{$\log(\Theta)$} & & \multicolumn{2}{c}{$E(44-55)$} & & \multicolumn{2}{c}{$R(55)$} & & \multicolumn{2}{c}{$d$} \\ "R+"\n");
() = fprintf(fp, "\cline{3-4} \cline{6-7} \cline{9-10} \cline{12-13} \cline{15-16} \cline{18-19} \cline{21-22} \cline{24-25} \cline{27-28} \cline{30-31} \cline{33-34}"R+"\n");
() = fprintf(fp, "& & & & & & & & & & & \multicolumn{2}{c}{$(M_{\odot})$} & & \multicolumn{2}{c}{(Myr)} & & & & & \multicolumn{2}{c}{$(R_{\odot})$} & & \multicolumn{2}{c}{(rad)} "R);
() = fprintf(fp, "& & \multicolumn{2}{c}{(mag)} & & & & & \multicolumn{2}{c}{(pc)} \\"R+"\n"+"\hline"R+"\n");
print_struct(fp, stell; sep=" & ", final=" \\"R, nohead);
() = fprintf(fp, "\hline"R+"\n"+"\end{tabular}"R+"\n"+"\end{document}"R+"\n");
() = fclose(fp);
() = system(sprintf("cd results; pdflatex -halt-on-error -file-line-error %s.tex | grep 'error' --color=always; rm %s.aux %s.log; cd ../", filename, filename, filename));

#endif

define tie_all(){
  % tie atm. parameters for all datasets
  _for i(1, len_comp, 1) % loop over components
  {
    variable field;
    foreach field(get_struct_field_names(spectrum_fit->params.macro_broad[i-1])){
      tie(sprintf("stellar(1).d1_c%d_",i)+field, sprintf("stellar(1).d*_c%d_",i)+field);
    }
    % tie(sprintf("stellar(1).d1_c%d_vrad",i), sprintf("stellar(1).d*_c%d_vrad",i));
    tie(sprintf("stellar(1).d1_c%d_teff",i), sprintf("stellar(1).d*_c%d_teff",i));
    tie(sprintf("stellar(1).d1_c%d_logg",i), sprintf("stellar(1).d*_c%d_logg",i));
    tie(sprintf("stellar(1).d1_c%d_xi",i), sprintf("stellar(1).d*_c%d_xi",i));
    tie(sprintf("stellar(1).d1_c%d_HE",i), sprintf("stellar(1).d*_c%d_HE",i));
    tie(sprintf("stellar(1).d1_c%d_z",i), sprintf("stellar(1).d*_c%d_z",i));
    foreach field(spectrum_fit->grid_info.species[i-1])
      tie(sprintf("stellar(1).d1_c%d_",i)+field, sprintf("stellar(1).d*_c%d_",i)+field);;
    tie(sprintf("stellar(1).d1_c%d_sur_ratio",i), sprintf("stellar(1).d*_c%d_sur_ratio",i));
    if(length(get_params("interstellar*"))>1)
    {
%      variable fields = get_params("interstellar*"); fields = array_struct_field(fields, "name");
      variable fields = {"t", "xi", "HI", "NaI", "CaII"};
      foreach field(fields)
        tie("interstellar(1).d1_"+field, "interstellar(1).d*_"+field);
    }
  };
  eval_counts;
}

define tie_blap(){
  % tie atm. parameters for all datasets
  _for i(1, len_comp, 1) % loop over components
  {
    variable field;
    foreach field(get_struct_field_names(spectrum_fit->params.macro_broad[i-1])){
      tie(sprintf("stellar(1).d1_c%d_",i)+field, sprintf("stellar(1).d*_c%d_",i)+field);
    }
    % tie(sprintf("stellar(1).d1_c%d_vrad",i), sprintf("stellar(1).d*_c%d_vrad",i));
%    tie(sprintf("stellar(1).d1_c%d_teff",i), sprintf("stellar(1).d*_c%d_teff",i));
%    tie(sprintf("stellar(1).d1_c%d_logg",i), sprintf("stellar(1).d*_c%d_logg",i));
    tie(sprintf("stellar(1).d1_c%d_xi",i), sprintf("stellar(1).d*_c%d_xi",i));
    tie(sprintf("stellar(1).d1_c%d_HE",i), sprintf("stellar(1).d*_c%d_HE",i));
    tie(sprintf("stellar(1).d1_c%d_z",i), sprintf("stellar(1).d*_c%d_z",i));
    foreach field(spectrum_fit->grid_info.species[i-1])
      tie(sprintf("stellar(1).d1_c%d_",i)+field, sprintf("stellar(1).d*_c%d_",i)+field);;
%    tie(sprintf("stellar(1).d1_c%d_sur_ratio",i), sprintf("stellar(1).d*_c%d_sur_ratio",i));
    if(length(get_params("interstellar*"))>1)
    {
%      variable fields = get_params("interstellar*"); fields = array_struct_field(fields, "name");
      variable fields = {"t", "xi", "HI", "NaI", "CaII"};
      foreach field(fields)
        tie("interstellar(1).d1_"+field, "interstellar(1).d*_"+field);
    }
  };
  eval_counts;
}

define tie_mag(){
  % tie atm. parameters for all components, except z (-> B field)
  tie("stellar(1).d1_c1_teff", "stellar(1).*_teff");
  tie("stellar(1).d1_c1_logg", "stellar(1).*_logg");
  tie("stellar(1).d1_c1_xi", "stellar(1).*_xi");
  tie("stellar(1).d1_c1_HE", "stellar(1).*_HE");
  tie("stellar(1).d1_c1_vsini", "stellar(1).*_vsini");
  tie("stellar(1).d1_c1_zeta", "stellar(1).*_zeta");
  _for i(1, len_comp, 1) % loop over components
  {
    _for id(1, len_sets, 1) % loop over datasets
    {
      tie(sprintf("stellar(1).d%d_c1_vrad", id), sprintf("stellar(1).d%d_c%d_vrad", id, i));
    }
  }
  _for i(1, len_comp, 1) % loop over components
  {
    tie(sprintf("stellar(1).d1_c%d_sur_ratio",i), sprintf("stellar(1).d*_c%d_sur_ratio",i));
    tie(sprintf("stellar(1).d1_c%d_z",i), sprintf("stellar(1).d*_c%d_z",i));
  };
  eval_counts;
}

define fit_cont_simple(){
  % fit only continuum
  variable temp = get_params("stellar(1).*");
  variable free = freeParameters; freeze(free); exclude(all_data);
  _for id(1, len_sets, 1) % loop over datasets
  {
    include(id);
    thaw(sprintf("csp*d%d_y*", id));
    set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
    exclude(id); freeze("*");
  };
  include(all_data); thaw(free); set_params(temp); __uninitialize(&temp);
  eval_counts;
}

define fit_cont()
  % fit only continuum, and freeze continuum in ignored regions
{
  variable free_init = freeParameters;
  variable ndata = length([specs]);
  exclude(all_data);
  _for id(1, ndata, 1) % loop over datasets
  {
    include(id);
    % Freeze continuum points in ignored regions:
    % ===========================================
    variable temp1 = get_params("stellar(1).*"); freeze("stellar(1).*");
    variable free = freeParameters; freeze(free);
    variable temp2 = get_data_info(id).notice_list;
    thaw(sprintf("cspline(1).d%d_y*", id));
    ignore(id); notice_list(id, temp2); () = eval_counts(; fit_verbose=-1);
    variable temp3 = length(get_par(sprintf("cspline(1).d%d_x*",id)));
    _for i(0, temp3-1, 1) % freeze continuum point 'i' when no noticed pixels are between anchorpoints 'i-1' and 'i+1'
    {
      variable temp4 = get_par(sprintf("cspline(1).d%d_x%d",id,max([0,i-1])));
      variable temp5 = get_par(sprintf("cspline(1).d%d_x%d",id,min([i+1,temp3-1])));
      variable xcurr = get_par(sprintf("cspline(1).d%d_x%d",id,i));
      variable dhi = (temp5 - xcurr);
      variable dlo = (xcurr - temp4);
      temp4 = temp4 + dlo / 1.5;
      temp5 = temp5 - dhi / 1.5;
      if(length(where(temp4<get_data_counts(id).bin_lo[temp2]<temp5))==0) % no noticed pixels between anchorpoints 'i-1' and 'i+1'
      {
        variable temp6;
        foreach temp6 (["x","y"])
        {
          variable temp7 = get_par_info(sprintf("cspline(1).d%d_%s%d",id,temp6,i));
          freeze(temp7.index);
          free = free[where(free!=temp7.index)];
          __uninitialize(&temp7);
        }
        __uninitialize(&temp6);
      }
      __uninitialize(&temp4); __uninitialize(&temp5);
    }
    __uninitialize(&temp2); __uninitialize(&temp3);
    set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
    set_params(temp1); __uninitialize(&temp1);
  }
  freeze("*");
  thaw(free_init);
}

define ignore_thres(id, thres_neg, thres_pos)
{
  % ignore datapoints outside sigma range
  include(id);
  variable original_notice = get_data_info(id).notice;
  variable original_ignore_list = where(original_notice==0);
  variable obs = get_data_counts(id);
  variable mo = get_model_counts(id);
  variable chi = (obs.value - mo.value) / obs.err;
  variable idx_chi_pos = where(chi > thres_pos);
  variable idx_chi_neg = where(chi < thres_neg);
  variable idx_ignore = union([idx_chi_pos, idx_chi_neg, original_ignore_list]);
  notice(id); ignore_list(id, idx_ignore); () = eval_counts(; fit_verbose=-1);
}

define fit_cont_UV(thres_neg, thres_pos)
  % fit only continuum, and freeze continuum in ignored regions
  % version for UV spectra (or spectra with lots of unmodeled lines)
{
  variable free_init = freeParameters;
  variable ndata = length([specs]);
  _for id(1, ndata, 1) % loop over datasets
  {
    exclude(all_data);
    include(id);
    variable original_notice = get_data_info(id).notice;
    variable original_notice_list = get_data_info(id).notice_list;
    variable original_ignore_list = where(original_notice==0);
    variable obs = get_data_counts(id);
    variable wave = obs.bin_lo;
    variable wmin = min(wave);
    if(wmin < 2300.){
      vmessage(sprintf("> fitting cont in dataset %d", id));
      ignore_thres(id, thres_neg, thres_pos);
      % ===========================================
      % freeze continuum point 'i' when no noticed pixels are between anchorpoints 'i-1' and 'i+1'
      variable temp1 = get_params("stellar(1).*"); freeze("stellar(1).*");
      variable free = freeParameters; freeze(free);
      thaw(sprintf("cspline(1).d%d_y*", id));
      variable temp3 = length(get_par(sprintf("cspline(1).d%d_x*",id)));
      _for i(0, temp3-1, 1)
      {
        variable temp4 = get_par(sprintf("cspline(1).d%d_x%d",id,max([0,i-1])));
        variable temp5 = get_par(sprintf("cspline(1).d%d_x%d",id,min([i+1,temp3-1])));
        variable xcurr = get_par(sprintf("cspline(1).d%d_x%d",id,i));
        variable dhi = (temp5 - xcurr);
        variable dlo = (xcurr - temp4);
        temp4 = temp4 + dlo / 1.5;
        temp5 = temp5 - dhi / 1.5;
         % no noticed pixels between anchorpoints 'i-1' and 'i+1'
        if(length(where(temp4<get_data_counts(id).bin_lo[get_data_info(id).notice_list]<temp5))==0)
        {
          variable temp6;
          foreach temp6 (["x","y"])
          {
            variable temp7 = get_par_info(sprintf("cspline(1).d%d_%s%d",id,temp6,i));
            freeze(temp7.index);
            free = free[where(free!=temp7.index)];
            __uninitialize(&temp7);
          }
          __uninitialize(&temp6);
        }
        __uninitialize(&temp4); __uninitialize(&temp5);
      }
      __uninitialize(&temp3);
      set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
      set_params(temp1); __uninitialize(&temp1);
      ignore(id);
      notice_list(id, original_notice_list);
    }
  }
  include(all_data);
  freeze("*");
  thaw(free_init);
  () = eval_counts();
}

define fit_cont_full()
{
  variable ndata = length([specs]);
  exclude(all_data);
  _for id(1, ndata, 1) % loop over datasets
  {
    include(id);
    % Freeze continuum points in ignored regions:
    % ===========================================
    variable temp1 = get_params("stellar(1).*"); freeze("stellar(1).*");
    variable free = freeParameters; freeze(free);
    variable temp2 = get_data_info(id).notice_list;
    thaw(sprintf("cspline(1).d%d_y*", id));
    notice(id); () = eval_counts(; fit_verbose=-1);
    vmessage(sprintf("fitting ignored+noticed d_%d:", id));
    set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
    ignore(id); notice_list(id, temp2); () = eval_counts(; fit_verbose=-1);
    variable temp3 = length(get_par(sprintf("cspline(1).d%d_x*",id)));
    _for i(0, temp3-1, 1) % freeze continuum point 'i' when no noticed pixels are between anchorpoints 'i-1' and 'i+1'
    {
      variable temp4 = get_par(sprintf("cspline(1).d%d_x%d",id,max([0,i-1])));
      variable temp5 = get_par(sprintf("cspline(1).d%d_x%d",id,min([i+1,temp3-1])));
      variable xcurr = get_par(sprintf("cspline(1).d%d_x%d",id,i));
      variable dhi = (temp5 - xcurr);
      variable dlo = (xcurr - temp4);
      temp4 = temp4 + dlo / 1.5;
      temp5 = temp5 - dhi / 1.5;
      if(length(where(temp4<get_data_counts(id).bin_lo[temp2]<temp5))==0) % no noticed pixels between anchorpoints 'i-1' and 'i+1'
      {
        variable temp6;
        foreach temp6 (["x","y"])
        {
          variable temp7 = get_par_info(sprintf("cspline(1).d%d_%s%d",id,temp6,i));
          freeze(temp7.index);
          free = free[where(free!=temp7.index)];
          __uninitialize(&temp7);
        }
        __uninitialize(&temp6);
      }
      __uninitialize(&temp4); __uninitialize(&temp5);
    }
    __uninitialize(&temp2); __uninitialize(&temp3);
    vmessage(sprintf("fitting noticed only, freezing ignored d_%d:", id));
    set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
    set_params(temp1); __uninitialize(&temp1);
  }
}

define fit_vrad(){
  % fit vrads + continuum
  variable free = freeParameters; freeze(free); exclude(all_data);
  _for id(1, len_sets, 1) % loop over datasets
  {
    include(id);
    thaw(sprintf("stellar(1).d%d_c*_vrad", id));
    thaw(sprintf("csp*d%d_y*", id));
    set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
    exclude(id); freeze("*");
  };
  include(all_data); thaw(free);
  eval_counts;
}

define fit_telluric(){
  % fit telluric model + continuum
  variable temp = get_params("stellar(1).*");
  variable free = freeParameters; freeze(free); exclude(all_data);
  _for id(1, len_sets, 1) % loop over datasets
  {
    include(id);
    thaw(sprintf("csp*d%d_y*", id));
    thaw(sprintf("telluric(1).d%d_airmass", id));
    thaw(sprintf("telluric(1).d%d_pwv", id));
    thaw(sprintf("telluric(1).d%d_barycorr", id));
    set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
    exclude(id); freeze("*");
  };
  include(all_data); thaw(free); set_params(temp); __uninitialize(&temp);
  eval_counts;
}

define disable_telluric()
{
  % disable telluric model
  set_par("*airmass*", 0, 1; min=0, max=3);
  freeze("*pwv*");
  freeze("*barycorr*");
}

define write_model(){
  variable ginfo = @spectrum_fit->grid_info;
  variable spec = @spectrum_fit->stellar_spectrum;
  variable spec_params = @spec.params;
  variable dimensions = array_shape(spec_params);
  variable ndatasets = dimensions[0];
  variable params = @spectrum_fit->params;
  params.HE = list_to_array(params.HE);
  variable nc = length(ginfo.location);
  variable s = struct{l, f};
  (s.l, s.f) = syn_spec(params.T,
                        params.G,
                        params.X,
                        params.Z,
                        params.HE,
                        ginfo;
                        macro_broad=params.macro_broad,
%                        vrad=0.,
%                        res_offset=res_offset, res_slope=res_slope,
%                        xmin=xmin, xmax=xmax,
                        sur_ratio=params.sur_ratio,
%                        nonorm
                        );
  variable sname = sprintf("results/model");
  if(qualifier_exists("ascii")){
     sname = sname + ".dat";
     fits_write_binary_table(sname, "Spectrum", s);
     writecol(sname, s.l, s.f);
  }
  else{
     sname = sname + ".fits";
     fits_write_binary_table(sname, "Spectrum", s);
  }
  vmessage(sprintf("> saved to %s", sname));
  return s;
}

define write_spec()
{
  % save model and best fit to .fits files
  % for each dataset (or to .ascii if "ascii" is set)
  variable s, outname;
  variable ndata = length([specs]);
  variable fit_cont = qualifier("fit_cont", 1);
%  variable nonorm = qualifier_exists("nonorm");
  exclude(all_data);
  _for id(1, ndata, 1) % loop over datasets
  {
    include(id);
    variable notice_original = @get_data_info(id).notice;
    % Freeze continuum points in ignored regions:
    % ===========================================
    if(fit_cont == 1)
    {
      variable temp1 = get_params("stellar(1).*"); freeze("stellar(1).*");
      variable free = freeParameters; freeze(free);
      variable temp2 = get_data_info(id).notice_list;
      thaw(sprintf("cspline(1).d%d_y*", id));
      notice(id); () = eval_counts(; fit_verbose=-1);
      list_free;
      print("fit all");
      set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
      ignore(id); notice_list(id, temp2); () = eval_counts(; fit_verbose=-1);

      variable temp3 = length(get_par(sprintf("cspline(1).d%d_x*",id)));
      _for i(0, temp3-1, 1) % freeze continuum point 'i' when no noticed pixels are between anchorpoints 'i-1' and 'i+1'
      {
        variable temp4 = get_par(sprintf("cspline(1).d%d_x%d",id,max([0,i-1])));
        variable temp5 = get_par(sprintf("cspline(1).d%d_x%d",id,min([i+1,temp3-1])));
        variable xcurr = get_par(sprintf("cspline(1).d%d_x%d",id,i));
        variable dhi = (temp5 - xcurr);
        variable dlo = (xcurr - temp4);
        temp4 = temp4 + dlo / 1.5;
        temp5 = temp5 - dhi / 1.5;
        if(length(where(temp4<get_data_counts(id).bin_lo[temp2]<temp5))==0) % no noticed pixels between anchorpoints 'i-1' and 'i+1'
        {
          variable temp6;
          foreach temp6 (["x","y"])
          {
            variable temp7 = get_par_info(sprintf("cspline(1).d%d_%s%d",id,temp6,i));
            freeze(temp7.index);
            free = free[where(free!=temp7.index)];
            __uninitialize(&temp7);
          }
          __uninitialize(&temp6);
        }
        __uninitialize(&temp4); __uninitialize(&temp5);
      }
      __uninitialize(&temp2); __uninitialize(&temp3);
      list_free;
      print("fit noticed only, freeze other");
      set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
      set_params(temp1); __uninitialize(&temp1);
    }

   notice(id); () = eval_counts(; fit_verbose=-1);

   variable data = get_data_counts(id);
   variable model = get_model_counts(id);
   variable factor = 1.;
   if(string_match(get_fit_fun, "stellar")!=0) variable cont = spectrum_fit->stellar_spectrum.c[id-1,-1];
%   if(string_match(get_fit_fun, "stellar")!=0 && nonorm==1) factor *= spectrum_fit->stellar_spectrum.c[id-1,-1];;
   if(string_match(get_fit_fun, "theta_interext")!=0) factor *= spectrum_fit->extinction_curve[id-1];
   if(string_match(get_fit_fun, "cspline")!=0) factor *= spectrum_fit->cspline_curve[id-1];;
   if(string_match(get_fit_fun, "telluric")!=0) factor *= spectrum_fit->telluric_spectrum[id-1];;
   if(qualifier_exists("noism") && string_match(get_fit_fun, "interstellar")!=0)
     factor *= spectrum_fit->interstellar_spectrum[id-1];;
   model.value = model.value / factor;
   variable corr = 1.+0.*model.value;
   % correct cont. in data, not model
   model.value /= corr;
   data.value /= (factor*corr);
   data.err /= (factor*corr);
   s = struct{l,f,cont,notice};
   (s.l, s.f, s.notice) = (model.bin_lo, model.value, notice_original);
   if(string_match(get_fit_fun, "cspline")!=0)
     s = struct_combine(s, struct{csp=spectrum_fit->cspline_curve[id-1]});
   if(qualifier_exists("ascii")){
     outname = sprintf("results/%s_d%d.dat", "model", id);
     if(struct_field_exists(s, "csp")){
       writecol(outname, s.l, s.f, s.csp, notice_original);
     }
     else{
       writecol(outname, s.l, s.f, notice_original);
     }
   }
   else{
     outname = sprintf("results/%s_d%d.fits", "model", id);
     fits_write_binary_table (outname, "Spectrum", s);
   }
   s = struct{l,f,e,fc,notice};
   (s.l, s.f, s.e, s.fc, s.notice) = (data.bin_lo, data.value, data.err, factor, notice_original);
   if(qualifier_exists("ascii")){
     outname = sprintf("results/%s_d%d.dat", "data", id);
     writecol(outname, s.l, s.f, s.e, s.notice);
   }
   else{
     outname = sprintf("results/%s_d%d.fits", "data", id);
     fits_write_binary_table (outname, "Spectrum", s);
   }
   exclude(id);
  }
  include(all_data);
  % restore original ignore list
  _for id(1, ndata, 1) % loop over datasets
  {
    variable col1, col2;
    (col1,col2) = readcol(sprintf("%slists/d%d_ignore_list.txt",wd,id), 1, 2);
   _for i(0, length(col1)-1, 1) ignore(id, col1[i], col2[i]);;
   () = eval_counts(; fit_verbose=-1);
  }
}

define fit_free(){
  set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
}

define save_quick(){
  save_par("params/params"); save_par("params/params_1");
}

define write_components(){
  variable i, j;
  variable ginfo = @spectrum_fit->grid_info;
  variable spec = @spectrum_fit->stellar_spectrum;
  variable spec_params = @spec.params;
  variable dimensions = array_shape(spec_params);
  variable ndatasets = dimensions[0];
  variable params = @spectrum_fit->params;
  params.HE = list_to_array(params.HE);
  variable nc = length(ginfo.location);
  variable fr = Double_Type[ndatasets,nc];
  % loop over components
  _for i(0, ndatasets-1, 1)
  {
    if(nc>1) variable slist = {};
    _for j(0, nc-1, 1)
    {

      variable params_c = struct_filter (params, j; dim=0, copy);
      variable ginfo_c = struct_filter (ginfo, j; dim=0, copy);
      ginfo_c.location = [ginfo_c.location];

      variable basestr = sprintf("d%d_c%d", i+1, j+1);
      variable T = get_par(sprintf("stellar(1).%s_teff", basestr));
      variable G = get_par(sprintf("stellar(1).%s_logg", basestr));
      variable X = get_par(sprintf("stellar(1).%s_xi", basestr));
      variable Z = get_par(sprintf("stellar(1).%s_z", basestr));
      variable HE = get_par(sprintf("stellar(1).%s_HE", basestr));
      variable sur_ratio = get_par(sprintf("stellar(1).%s_sur_ratio", basestr));
      variable res_offset = get_par(sprintf("stellar(1).d%d_res_offset", i+1));
      variable res_slope = get_par(sprintf("stellar(1).d%d_res_slope", i+1));
      variable vsini = get_par(sprintf("stellar(1).%s_vsini", basestr));
%      variable zeta = get_par(sprintf("stellar(1).%s_zeta", basestr));
%      variable zeta = 0.;
      variable vrad = get_par(sprintf("stellar(1).%s_vrad", basestr));
%      variable macro_broad = params_c.macro_broad;
      variable macro_broad = struct{vsini=vsini};

      if(qualifier_exists("nores"))
      {
        res_offset = 1e6;
        res_slope = 0;
      }
%      variable vsini = spec_params[i,j][2];
%      variable zeta = spec_params[i,j][3];
%      variable vrad = spec_params[i,j][4]; % params_c.vrad
      variable wave_obs = spec.l[i,0];
      variable wave_obs_noticed = spec.l[i,1];
      variable xmin = min(spec.l[i,0]);
      variable xmax = max(spec.l[i,0]);
      if(qualifier_exists("nolim"))
      {
        xmin = 0;
        xmax = _Inf;
      }
      variable s = struct{l, f};
      variable sc = struct{l, f};
      if(qualifier_exists("metals")){
        (s.l, s.f) = syn_spec([T],
                              [G],
                              [X],
                              [Z],
                              [HE],
                              ginfo_c;
                              macro_broad=[macro_broad],
                              vrad=[0],
                              res_offset=res_offset, res_slope=res_slope,
                              xmin=xmin, xmax=xmax,
                              sur_ratio=[sur_ratio],
                              metals=[params_c.metals],
                              nonorm);
        (sc.l, sc.f) = syn_spec([T],
                                [G],
                                [X],
                                [Z],
                                [HE],
                                ginfo_c;
                                macro_broad=[macro_broad],
                                vrad=[0],
                                res_offset=res_offset, res_slope=res_slope,
                                xmin=xmin, xmax=xmax,
                                sur_ratio=[sur_ratio],
                                metals=[params_c.metals]);
      }
      else{
        (s.l, s.f) = syn_spec([T],
                              [G],
                              [X],
                              [Z],
                              [HE],
                              ginfo_c;
                              macro_broad=[macro_broad],
                              vrad=[0],
                              res_offset=res_offset, res_slope=res_slope,
                              xmin=xmin, xmax=xmax,
                              sur_ratio=[sur_ratio],
                              nonorm);
        (sc.l, sc.f) = syn_spec([T],
                                [G],
                                [X],
                                [Z],
                                [HE],
                                ginfo_c;
                                macro_broad=[macro_broad],
                                vrad=[0],
                                res_offset=res_offset, res_slope=res_slope,
                                xmin=xmin, xmax=xmax,
                                sur_ratio=[sur_ratio]);
      }
      if(qualifier_exists("nolim") or qualifier_exists("nores"))
      {
        variable sname = sprintf("results/d%d_c%d_raw.fits", i+1, j+1);
        fits_write_binary_table (sname, "Spectrum", s);
      }
      variable vrad_factor = (1. + vrad/299792.458);

      % bring to observed wavelength grid
      s.f = interpol(wave_obs, s.l*vrad_factor, s.f);
      s.l = wave_obs;
      sc.f = interpol(wave_obs, sc.l*vrad_factor, sc.f);
      sc.l = wave_obs;

      variable fcont = s.f / sc.f;
      if(nc>1){
        variable fsum;
        if(j==0){
          fsum = fcont;}
        else{
          fsum += fcont;}
        list_append(slist, s);
      }
      else{
        sname = sprintf("results/d%d_c%d", i+1, j+1);
        if(qualifier_exists("ascii"))
          writecol(sname+".dat", s.l, s.f, fsum);
        else
          fits_write_binary_table (sname+".fits", "Spectrum", s);
        vmessage(sprintf("> saved to %s", sname));
      }
    }
    if(nc>1){
      _for j(0, nc-1, 1)
      {
        sname = sprintf("results/d%d_c%d", i+1, j+1);
        s = slist[j];
        s.f /= fsum;
        variable print_flux_contrib = 5500.;
        if(print_flux_contrib>0){
          variable idx_ran = where(abs(s.l-print_flux_contrib) < 10);
          variable fr_d = mean(s.f[idx_ran]);
          fr[i,j] = fr_d;
          vmessage(sprintf("c%d at %.0f: %.4f", j+1, print_flux_contrib, fr_d));
        }
        if(qualifier_exists("ascii"))
          writecol(sname+".dat", s.l, s.f, fsum);
        else
          fits_write_binary_table(sname+".fits", "Spectrum", s);
        vmessage(sprintf("> saved to %s", sname));
      }
    }
  }
  if(nc>1){
    variable sfr = @Struct_Type ( ["fr_c1", "fr_c2"] );
    sfr.fr_c1 = fr[[0:array_shape(fr)[0]-1], 0];
    sfr.fr_c2 = fr[[0:array_shape(fr)[0]-1], 1];
    variable sname_av = "fr.dat";
    print_struct(sname_av, sfr);
    vmessage(sprintf("> saved to %s", sname_av));
  }
}

define read_mjd_from_fits(){
  variable spec = @spectrum_fit->stellar_spectrum;
  variable spec_params = @spec.params;
  variable dimensions = array_shape(spec_params);
  variable ndatasets = dimensions[0];
  variable wd = getcwd;
  variable mjds = {};
  variable i;
  _for i(0, ndatasets-1, 1) % loop over datasets
  {
    variable fname = sprintf("%sfitsfiles/d%d_spectrum.fits.gz[1]", wd, i+1);
    variable mjd = fits_read_key(fname, "MJD");
    list_append(mjds, mjd);
  }
  return list_to_array(mjds);
}

define read_key_from_fits(key){
  variable spec = @spectrum_fit->stellar_spectrum;
  variable spec_params = @spec.params;
  variable dimensions = array_shape(spec_params);
  variable ndatasets = dimensions[0];
  variable wd = getcwd;
  variable mjds = {};
  variable i;
  _for i(0, ndatasets-1, 1) % loop over datasets
  {
    variable fname = sprintf("%sfitsfiles/d%d_spectrum.fits.gz[1]", wd, i+1);
    variable mjd = fits_read_key(fname, key);
    list_append(mjds, mjd);
  }
  return list_to_array(mjds);
}

define save_vrad(){
  % save vrads to table
  variable ginfo = @spectrum_fit->grid_info;
  variable nc = length(ginfo.location);
  variable spec = @spectrum_fit->stellar_spectrum;
  variable spec_params = @spec.params;
  variable dimensions = array_shape(spec_params);
  variable ndatasets = dimensions[0];
  variable vrads = Double_Type[ndatasets, nc];
  variable teffs = Double_Type[ndatasets, nc];
  variable loggs = Double_Type[ndatasets, nc];
  variable zs = Double_Type[ndatasets, nc];
  variable sur_ratios = Double_Type[ndatasets, nc];
  variable j, i;
  variable v = struct{"index", "c1_vrad", "c1_teff", "c1_logg", "c1_z", "c1_sur_ratio"};
  _for j(0, nc-1, 1){
    v = struct_combine(v, sprintf("c%d_vrad", j+1));
    v = struct_combine(v, sprintf("c%d_teff", j+1));
    v = struct_combine(v, sprintf("c%d_logg", j+1));
    v = struct_combine(v, sprintf("c%d_z", j+1));
    v = struct_combine(v, sprintf("c%d_sur_ratio", j+1));
    _for i(0, ndatasets-1, 1){
      variable basestr = sprintf("d%d_c%d", i+1, j+1);
%      vrads[i,j] = spec_params[i,j][4];
      vrads[i,j] = get_par(sprintf("stellar(1).%s_vrad", basestr));;
      teffs[i,j] = get_par(sprintf("stellar(1).%s_teff", basestr));;
      loggs[i,j] = get_par(sprintf("stellar(1).%s_logg", basestr));;
      zs[i,j] = get_par(sprintf("stellar(1).%s_z", basestr));;
      sur_ratios[i,j] = get_par(sprintf("stellar(1).%s_sur_ratio", basestr));;
    }
    set_struct_field(v, sprintf("c%d_vrad", j+1), vrads[*,j]);
    set_struct_field(v, sprintf("c%d_teff", j+1), teffs[*,j]);
    set_struct_field(v, sprintf("c%d_logg", j+1), loggs[*,j]);
    set_struct_field(v, sprintf("c%d_z", j+1), zs[*,j]);
    set_struct_field(v, sprintf("c%d_sur_ratio", j+1), sur_ratios[*,j]);
  }
  set_struct_field(v, "index", [1:ndatasets:1]);
  if(qualifier_exists("mjd")){
    v = struct_combine("mjd", v);
    v.mjd = read_key_from_fits("MJD");
  }
  if(qualifier_exists("fpath")){
    v = struct_combine(v, "fpath");
    v.fpath = read_key_from_fits("FPATH");
  }
  %
  variable thres_av_time = qualifier("average", 0);
  if(thres_av_time==NULL) thres_av_time = 10.;
  if((thres_av_time>0) && qualifier_exists("mjd")){
    variable v_av = struct{mjd={}, c1_vrad={}, c1_vrad_err={}};
    _for i(0, ndatasets-1, 1){
      % exposures that are closer than 10s
      variable idx = where(abs(v.mjd-v.mjd[i])*24*60^2 < thres_av_time);
      list_append(v_av.mjd, mean(v.mjd[idx]));
      list_append(v_av.c1_vrad, mean(v.c1_vrad[idx]));
      list_append(v_av.c1_vrad_err, moment(v.c1_vrad[idx]).sdev);
    }
    v_av.mjd = list_to_array(v_av.mjd);
    v_av.c1_vrad = list_to_array(v_av.c1_vrad);
    idx = unique(v_av.mjd);
    v_av.mjd = v_av.mjd[idx];
    v_av.c1_vrad = v_av.c1_vrad[idx];
    variable sname_av = "vrads_av.dat";
    print_struct(sname_av, v_av);
    vmessage(sprintf("> saved to %s", sname_av));
  }
  variable sname = "vrads.dat";
  print_struct(sname, v);
  vmessage(sprintf("> saved to %s", sname));
}

define plot_spec(){
  % create PDF plots
  variable ndata = length(all_data);
  variable spath = qualifier("spath", "residual_plot_all.pdf");
  variable xrange = qualifier("xrange", 1550.);
  variable xmin = qualifier("xmin", -1.0);
  variable xmax = qualifier("xmax", -1.0);
  variable qualies_for_xfig_residual_plot = __qualifiers;
  variable j, k;
  % use either 'll' or 'linelist' to specify linelists
  if(struct_field_exists(qualies_for_xfig_residual_plot, "ll"))
  {
    qualies_for_xfig_residual_plot = struct_combine(qualies_for_xfig_residual_plot,
                                                    struct{linelist=qualies_for_xfig_residual_plot.ll});
    if(typeof(qualies_for_xfig_residual_plot.linelist)==List_Type){
      _for j(0, ndata-1, 1){
        variable lj = @qualies_for_xfig_residual_plot.linelist[j];
        if(typeof(lj)==Array_Type){
            qualies_for_xfig_residual_plot.linelist[j] = lj[0];
            lj = lj[0];
        }
        if(struct_field_exists(lj, "reliability")) struct_filter(lj, where(lj.reliability=="good"); dim=0);
        variable bstr = sprintf("d%d_c1", 1);
        variable teff = get_par(sprintf("stellar(1).%s_teff", bstr));
        if(teff<25000.)
          struct_filter(lj, where(lj.element+lj.ion_level!="HeII"); dim=0);
        qualies_for_xfig_residual_plot.linelist[j] = lj;
      }
    }
    else{
      lj = @qualies_for_xfig_residual_plot.linelist;
      if(struct_field_exists(lj, "reliability")) struct_filter(lj, where(lj.reliability=="good"); dim=0);
      qualies_for_xfig_residual_plot.linelist = lj;
    }
  }
  variable pdftk = "pdftk";
  system("echo '' > pdftk.txt");
  _for j(0, ndata-1, 1){
    variable xmin_d = get_data_counts(j+1).bin_lo[0];
    variable xmax_d = get_data_counts(j+1).bin_lo[-1];
    variable vrad_c1_d = get_par(sprintf("stellar(1).d%d_c1_vrad", j+1));
    if(xmax>0)
      xmax_d = _min(xmax_d, xmax);
    if(xmin>0)
      xmin_d = _max(xmin_d, xmin);
    variable xborders;
    if(typeof(qualifier("xrange"))==String_Type){
      xrange = xmax_d - xmin_d;
      xborders = union([[xmin_d:xmax_d:xrange], xmax_d]);
    }
    else if(typeof(qualifier("xrange"))==Integer_Type){
      variable npanels = qualifier("xrange");
      xborders = [xmin_d:xmax_d:#npanels+1];
      xrange = (-xmin_d+xmax_d)/npanels;
    }
    else if(typeof(qualifier("xrange"))==Array_Type){
      xborders = qualifier("xrange");
      if(typeof(xborders[0])==Integer_Type){
        npanels = qualifier("xrange")[j];
        xborders = [xmin_d:xmax_d:#npanels+1];
        xrange = (-xmin_d+xmax_d)/npanels;
      }
      else{
        xrange = (xrange[-1] - xrange[0]) / (length(xborders)-1);
      }
    }
    else{
      xborders = union([[xmin_d:xmax_d:xrange], xmax_d]);
    }
    variable nb = length(xborders);
    if(nb>2) xborders[-1] = xborders[-2]+xrange;
    variable pdftk_d = "pdftk";
    _for k(0, nb-2, 1)
    {
      if( (xmax_d>=xborders[k] and xmax_d<xborders[k+1]) ||
          (xmin_d<=xborders[k+1] and xmin_d>xborders[k]) ||
          (xmin_d<=xborders[k] and xmax_d>=xborders[k+1]) )
      {
        variable qualies_for_xfig_residual_plot_d = struct_combine(qualies_for_xfig_residual_plot,
                                                        struct{xmin=xborders[k],
                                                               xmax=xborders[k+1]});
        % this would shift the obs. spec. and model to RV = 0;
%        qualies_for_xfig_residual_plot = struct_combine(qualies_for_xfig_residual_plot,
%                                                        struct{correct_vrad=vrad_c1_d});
        if(struct_field_exists(qualies_for_xfig_residual_plot_d, "linelist")){
          variable ll_d = @get_struct_field(qualies_for_xfig_residual_plot, "linelist");
          if(typeof(ll_d)==Struct_Type){
            ll_d = struct_combine(ll_d, struct{vrad=vrad_c1_d});
          }
          else{
            ll_d = struct_combine(ll_d[j], struct{vrad=vrad_c1_d});
%            qualies_for_xfig_residual_plot_d.linelist[j] = ll_d;
          }
          struct_filter (ll_d, where(ll_d.wavelength>xborders[k] and ll_d.wavelength<xborders[k+1]); dim=0);
          variable nthres = 95;
          if(length(ll_d.wavelength)>nthres){
            if(struct_field_exists(ll_d, "ew")){
              variable ew_thresh = min(ll_d.ew)*1.2;
              variable idxt = where(ll_d.ew>ew_thresh);
              variable iter = 0;
              while(length(idx)>=nthres and iter<50){
                ew_thresh *= 1.1;
                idxt = where(ll_d.ew>ew_thresh);
                iter++;
              }
              if(length(idxt)>=nthres)
                idxt = idxt[[0:nthres-1]];
              struct_filter (ll_d, idxt; dim=0);
            }
            else
              struct_filter (ll_d, [0:nthres-1]; dim=0);
          }
          qualies_for_xfig_residual_plot_d.linelist = ll_d;
        }
        variable fig = xfig_residual_plot(j+1;; qualies_for_xfig_residual_plot_d);
        variable spath_curr = sprintf("d%d_p%d_%s", j+1, k, spath);
        fig.render(spath_curr);
        pdftk = sprintf("%s %s", pdftk, spath_curr);
        pdftk_d = sprintf("%s %s", pdftk_d, spath_curr);
      }
    }
    if(pdftk_d!="pdftk")
    {
      variable spath_d = sprintf("residual_plot_d%d.pdf", j+1);
      pdftk_d = sprintf("%s cat output %s", pdftk_d, spath_d);
      system(sprintf("echo '%s' >> pdftk.txt", pdftk_d));
      variable err = system(pdftk_d);
      if(err==0) vmessage(sprintf("> saved to %s", spath_d));
    }
  }
  if(pdftk!="pdftk")
  {
    pdftk = sprintf("%s cat output %s", pdftk, spath);
    system(sprintf("echo '%s' >> pdftk.txt", pdftk));
    err = system(pdftk);
    if(err==0) vmessage(sprintf("> saved to %s", spath));
  }
}

define read_fort12(fpath, ew_min){
  % read synspec fort.12 file into 'struct'
  variable s, igood, rel, rbad, i;
  s = ascii_read_table(fpath, [{"%s",""}, {"%s",""}, {"%F","wavelength"},
                               {"%s","element"}, {"%s","ion_level"},
                               {"%s",""}, {"%s",""}, {"%s",""}, {"%F","ew"}]);
  %
  % EWs are wrong in Balmer line cores
  rbad = {{3700.,3705.},{3966.,3975.},{4023.,4030.},
          {4100.,4105.},{4337.,4343.},{4682.,4689.},
          {4850.,4880.},{6555.,6573.},{8430.,8443.},
          {8460.,8475.},{8500.,8515.},{8543.,8560.},
          {8585.,8610.},{8631.,8640.},
          {8655.,8662.},{8663.,8670.},{8740.,8760.},
          {8780.,8850.},{8850.,8880.},{8880.,9030.},
          {9070.,9224.},{9223.,9250.},{9270.,9350.}};
  %
  _for i(0, length(rbad)-1, 1){
    igood = where((s.wavelength < rbad[i][0]) or (s.wavelength > rbad[i][1]));
    struct_filter (s, igood; dim=0);
  }
  %
  if(qualifier_exists("addhe2")){
    variable s2 = struct{wavelength, element, ion_level, ew};

    s2.wavelength = [3203.10, 4685.71, % # 3-X
                     6560.10, 5411.52, 4859.319, % 4-X
                     4541.591, 4338.671, 4199.832, 4100.041,
                     4025.601, 3968.432, 3923.481, 3887.442];
    variable nline = length(s2.wavelength);
    s2.ew = 200. + Double_Type[nline];
    s2.element = String_Type[nline];
    s2.element[[0:nline-1]] = "He";
    s2.ion_level = String_Type[nline];
    s2.ion_level[[0:nline-1]] = "II";
    s.wavelength = [s.wavelength, s2.wavelength];
    s.element = [s.element, s2.element];
    s.ion_level = [s.ion_level, s2.ion_level];
    s.ew = [s.ew, s2.ew];
  }
  %
  if(qualifier_exists("addh")){
    s2 = struct{wavelength, element, ion_level, ew};
    s2.wavelength = [18751.01,12818.08,10938.095,10049.373,
                     9545.971,9229.015,9014.910,8862.783,
                     8750.472,8665.018,8598.392,
                     8545.383,8502.483,8467.253, % 3-X Paschen
                     6562.80,4861.325,4340.463,4101.735,
                     3970.072,3889.049,3835.384,3797.897,
                     3770.630]; % 2-X Balmer
    nline = length(s2.wavelength);
    s2.ew = 200. + Double_Type[nline];
    s2.element = String_Type[nline];
    s2.element[[0:nline-1]] = "H";
    s2.ion_level = String_Type[nline];
    s2.ion_level[[0:nline-1]] = "I";
    s.wavelength = [s.wavelength, s2.wavelength];
    s.element = [s.element, s2.element];
    s.ion_level = [s.ion_level, s2.ion_level];
    s.ew = [s.ew, s2.ew];
  }
  %
  igood = where(s.ew > ew_min);
  struct_filter (s, igood; dim=0);
  %
  rel = String_Type[length(s.ew)];
  rel[[0:length(rel)-1]] = "good";
  s = struct_combine (s, struct{reliability=rel});
  %
  variable vrad = qualifier("vrad", 0.);
  if(vrad!=0.){
    variable c = 299792.458; % km/s
    variable fac_vrad = sqrt((1.+(vrad/c))/(1.-(vrad/c)));
    s.wavelength *= fac_vrad;
  }
  return s;
}

define ec(){
  % simple shortcut
  eval_counts;
}

define fit_quick()
{
  % simple shortcut to fit all free parameters,
  % save them, and list them
  fit_free;
  save_quick;
  variable free = freeParameters;
  if(length(free)>0)
  {
    variable temp = get_par_info(free[0]);
    if(string_match(temp.name, "cspline"))
      vmessage("> skipped listing free cspline(s)");
  }
  freeze("csp*");
  list_free;
  thaw(free); __uninitialize(&free);
  __uninitialize(&temp);
}

define compute_uncertainties()
{
  % Compute 1-sigma statistical uncertainties for "stellar" parameters.
  % Set qualifier "conf" use "conf_loop", otherwise from covariances.

  variable fp_save = qualifier("fp_save", "results/results_conf.fits");
  variable chi_thres = qualifier("chi_thres", -10.);
  if(chi_thres>0)
  {
    rescale_errors(; chi_thres=chi_thres);
    % add_systematic_errors();
  }

  if(not qualifier_exists("conf"))
  {
    variable fit_stats;
    set_fit_method("mpfit"); % provides access to the covariance matrix
    variable fit_failed = fit_counts(&fit_stats; fit_verbose=-1);
    variable temp = get_par_info(freeParameters(; fit_fun_component="stellar"));
    variable conf_min = Double_Type[length(temp)];
    variable conf_max = Double_Type[length(temp)];
    variable i;
    _for i(0, length(temp)-1, 1)
    {
      % the square root of the diagonal elements are the uncertainties;
      % the parameters of the fit-function component 'stellar'
      % appear first in the covariance matrix
      variable uncertainty = sqrt(fit_stats.covariance_matrix[i,i]);
      conf_min[i] = temp[i].value - uncertainty;
      conf_max[i] = temp[i].value + uncertainty;
    }
    __uninitialize(&temp);
  }
  else
  {
    (conf_min, conf_max) = conf_loop(freeParameters(; fit_fun_component="stellar"), 0, 1e-1; num_slaves=1);
    save_par("params/params"); save_par("params/params_5");
  }
  variable stat; () = eval_counts(&stat; fit_verbose=-1);
  variable cs = conf_loop_summary(freeParameters(; fit_fun_component="stellar"), conf_min, conf_max);
  _for i(0, length(cs.index)-1, 1)
  {
    vmessage(sprintf("%s\t=\t%12f - %12f + %12f", cs.name[i], cs.value[i],
             cs.value[i]-cs.conf_min[i], -cs.value[i]+cs.conf_max[i]));
  }
  fits_write_binary_table(fp_save, "pvm_fit_pars-results", cs,
                          struct{chi2 = stat.statistic, num_bins = stat.num_bins,
                                 n_var_pars = stat.num_variable_params,
                                 dof = stat.num_bins-stat.num_variable_params,
                                 chi2red = stat.statistic/(stat.num_bins-stat.num_variable_params)});
}

define remove_telluric_model()
{
    fit_fun(strreplace(get_fit_fun,"*telluric","")); () = eval_counts;
}

define remove_telluric()
{
  if(string_match(get_fit_fun, "telluric")!=0)
  {
    variable temp = get_params("stellar*"); freeze("stellar*");
    variable free = freeParameters; freeze(free); exclude(all_data);
    variable conf_ind = Integer_Type[0], conf_min = Double_Type[0], conf_max = Double_Type[0];
    _for id(1, len_sets, 1) % loop over datasets
    {
      include(id);
      thaw(sprintf("csp*d%d_y*", id));
      thaw(sprintf("telluric(1).d%d_airmass", id));
      thaw(sprintf("telluric(1).d%d_pwv", id));
      thaw(sprintf("telluric(1).d%d_barycorr", id));
      % thaw previously free parameters containing the string ".d$id_" in their name
      thaw(free[(where(array_map(Integer_Type, &string_match, array_map(String_Type, &get_struct_field, get_params(free), "name"),
                sprintf(".d%d_",id))!=0))]);
      () = fit_counts;
      variable temp1, temp2, temp3 = freeParameters(; fit_fun_component="telluric");
      if(length(temp3)>0)
      {
        (temp1, temp2) = conf_loop(temp3, 0, 1e-1; num_slaves=1, max_num_retries=100);
        conf_min = [conf_min, temp1]; conf_max = [conf_max, temp2]; conf_ind = [conf_ind, temp3];
        __uninitialize(&temp1); __uninitialize(&temp2); __uninitialize(&temp3);
      }
      exclude(id); freeze("*");
    }
    temp1 = where(conf_min==conf_max);
    % when 'conf_loop' encounters more than 'max_num_retries'-times an improved fit, it will return 0 for conf_min and conf_max
    conf_min[temp1] = get_par(conf_ind[temp1]); conf_max[temp1] = get_par(conf_ind[temp1]);
    fits_write_binary_table("results/results_telluric_conf.fits", "pvm_fit_pars-results",
                            conf_loop_summary(conf_ind, conf_min, conf_max));
    include(all_data); thaw(free); set_params(temp); __uninitialize(&temp); __uninitialize(&temp1);
    save_par("params/params"); save_par("params/params_1");
    _for id(1, len_sets, 1) % loop over datasets
    {
      temp = get_data_counts(id);
      temp.value /= spectrum_fit->telluric_spectrum[id-1];
      temp.err /= spectrum_fit->telluric_spectrum[id-1]; % to conserve the chi^2 statistics
      put_data(id, temp); % replace dataset
      __uninitialize(&temp);
      fits_write_binary_table(sprintf("%sfitsfiles/d%d_spectrum.fits", wd, id), "Spectrum",
                  struct_combine(fits_read_table(sprintf("%sfitsfiles/d%d_spectrum.fits.gz", wd, id)),
                  struct{telluric_spectrum=spectrum_fit->telluric_spectrum[id-1]}, get_data_counts(id)));
      () = system(sprintf("gzip -f %sfitsfiles/d%d_spectrum.fits", wd, id));
    }
    fit_fun(strreplace(get_fit_fun,"*telluric","")); () = eval_counts;
    save_par("params/params"); save_par("params/params_2");
  };
}

% Specific linelist, required for local normalisation
define create_specific_linelists(ll)
{
  variable absent_chi_threshold = qualifier("absent_chi_threshold", 2.);
  % restart script to load default linelist as input for 'create_specific_linelist'
  () = system("rm -f lists/d*linelist.fits"); () = evalfile("./"+__argv[1], current_namespace);
  _for id(0, len_sets-1, 1)
  {
    ll[id] = create_specific_linelist(id+1, ll[id];
                                      save="lists/", absent_chi_threshold=absent_chi_threshold);
  }
  return ll;
}

define save_local_continuum(cc)
{
  _for id(1, len_sets, 1) % loop over datasets
  {
    variable temp = get_data_counts(id);
    variable old = fits_read_table(sprintf("%sfitsfiles/d%d_spectrum.fits.gz", wd, id));
    % combine old and new continuum correction
    if(struct_field_exists(old,"continuum_correction"))
    {
      cc[id-1] *= old.continuum_correction;
      temp.value *= old.continuum_correction;
      temp.err *= mean(old.continuum_correction);
    }
    temp.value /= cc[id-1];
    temp.err /= mean(cc[id-1]); % to roughly conserve the chi^2 statistics
    put_data(id, temp); % replace dataset
    __uninitialize(&temp);
    fits_write_binary_table(sprintf("%sfitsfiles/d%d_spectrum.fits", wd, id), "Spectrum",
      struct_combine(old, struct{continuum_correction=cc[id-1]}, get_data_counts(id)));
    () = system(sprintf("gzip -f %sfitsfiles/d%d_spectrum.fits", wd, id));
  };
}

% Local normalization: (useful for detailed abundance studies based on high-resolution spectra)
define normalise_local(ll)
{
  variable cc = continuum_correction(ll; interactive);
  save_local_continuum(cc);
}

define line_analyses()
{
  % create_specific_linelists must be run first!
  % line_analysis creates detailed line lists, saved to lists/
  variable oa, dla;
  _for id(1, len_sets, 1) % loop over datasets
    (oa, dla) = line_analysis(id, ll[id-1]; save="lists/");;
}

define apply_mask(ll)
% first run:
% ll = create_specific_linelists(ll);
% apply_mask(ll; save, hydrogen=0.6);
{
  variable qualies = __qualifiers;
  _for id(0, len_sets-1, 1) % loop over datasets
  {
    if(struct_field_exists(qualies, "save"))
    {
      qualies = struct_combine(qualies,
                  struct{ignore_list=sprintf("%slists/d%d_ignore_list.txt", wd, id+1)});
    }
    variable mask = Struct_Type[len_comp];
    _for i(0, len_comp-1, 1) % loop over components
    {
      % add absent stellar lines in the given linelist to the spectral mask
      ind = where(ll[id][i].reliability=="absent" and ll[id][i].ion_level!="IS");
      % also add the cores of the Balmer lines and helium lines with forbidden components to
      % the spectral mask if the effective temperature is larger than 12000 K
      if(get_par(sprintf("stellar(1).d%d_c%d_teff",id+1,i+1))>12000)
        ind = union(ind, where((ll[id][i].element=="H" and ll[id][i].wavelength<7000) or
                               (ll[id][i].element=="He" and ll[id][i].reliability=="bad")));
      mask[i] = struct_filter(ll[id][i], ind; copy);
      % sort the lines in the mask in ascending order
      struct_filter(mask[i], array_sort(mask[i].wavelength));
    }
    create_ignore_list_from_spectral_mask(id+1, mask;; qualies);
  }
}

define freeze_ignored()
% Freeze continuum points in ignored regions
{
  if(string_match(get_fit_fun, "cspline")!=0)
  {
    variable temp1 = get_params("stellar(1).*"); freeze("stellar(1).*");
    variable free = freeParameters; freeze(free); exclude(all_data);
    _for id(1, len_sets, 1) % loop over datasets
    {
      variable temp2 = get_data_info(id).notice_list;
      include(id); notice(id);
      % thaw previously free parameters containing the string ".d$id_" in their name
      thaw(free[(where(array_map(Integer_Type, &string_match, array_map(String_Type, &get_struct_field, get_params(free), "name"),
                                sprintf(".d%d_",id))!=0))]);
      set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
      ignore(id); notice_list(id, temp2); () = eval_counts(; fit_verbose=-1);
      variable temp3 = length(get_par(sprintf("cspline(1).d%d_x*",id)));
      % freeze continuum point 'i' when no noticed pixels are between anchorpoints 'i-1' and 'i+1'
      _for i(0, temp3-1, 1)
      {
        variable temp4 = get_par(sprintf("cspline(1).d%d_x%d",id,max([0,i-1])));
        variable temp5 = get_par(sprintf("cspline(1).d%d_x%d",id,min([i+1,temp3-1])));
        % no noticed pixels between anchorpoints 'i-1' and 'i+1'
        if(length(where(temp4<get_data_counts(id).bin_lo[temp2]<temp5))==0)
        {
          variable temp6;
          foreach temp6 (["x","y"])
          {
            variable temp7 = get_par_info(sprintf("cspline(1).d%d_%s%d",id,temp6,i));
            freeze(temp7.index);
            free = free[where(free!=temp7.index)];
            __uninitialize(&temp7);
          }
          __uninitialize(&temp6);
        }
        __uninitialize(&temp4); __uninitialize(&temp5);
      }
      __uninitialize(&temp2); __uninitialize(&temp3);
      set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
      exclude(id); freeze("*");
    }
    include(all_data); thaw(free); set_params(temp1); __uninitialize(&temp1);
    save_par("params/params"); save_par("params/params_1");
  };
}

define reload_file()
{
  () = evalfile("./"+__argv[1], current_namespace);
}

define reload_ignore()
{
  exclude(all_data);
  % loop over datasets
  _for id(1, len_sets, 1)
  {
    include(id); notice(id);
    (col1,col2) = readcol(sprintf("%slists/d%d_ignore_list.txt",wd,id), 1, 2);
    _for i(0, length(col1)-1, 1)
      ignore(id, col1[i], col2[i]);
  }
  include(all_data);
}

define link_ignore(){
  % make all ignore lists symbolic links to the first one
  variable ndata = length([specs]);
  variable id;
  _for id(2, ndata, 1)
  {
    system(sprintf("ln -sf d1_ignore_list.txt lists/d%d_ignore_list.txt", id));
  }
}

define cheatsheet()
{
  variable helptext = %{{{
    `
    help hotkeys
    hotkeys;

    list_par;
    set_par("*teff", 23000);
    "evaluate model";
    ec;

    fit_cont;
    save_quick;
    fit_vrad;

    tie_all();

    freeze("*");
    thaw("*teff");
    fit_quick;

    thaw("*logg");
    thaw("*HE");
    fit_quick;

    thaw("c*y*");
    fit_quick;

    fit_telluric;
    "fit and then remove telluric lines";
    remove_telluric;
    "only remove the telluric model";
    remove_telluric_model;

    hotkeys(; linelist=ll);
    "cut out outliers with key 'e'";
    "exit with 'Q'";
    "see also help hotkeys";

    add_systematic_errors;
    fit_quick; fit_quick;

    list_free;
    list_par;

    write_spec(; ascii);

    freeze("c*y*");

    "so that rchi^2 = 1";
    rescale_errors(; chi_thres=10.);
    "use covariances";
    compute_uncertainties();
    "use confidence intervals";
    compute_uncertainties(; conf);

    "for binary stars";
    "define a mass ratio q for circular orbits";
    derive_vrad();

    "fit spectra individually";
    fit_vrad;
    fit_individual;
    add_systematic_errors;
    fit_individual;
    uncertainties_individual;
    "ignore the same regions for each spectrum";
    link_ignore;
    reload_ignore;

    "only for metal analysis + high quality spectra";
    save_quick;
    ll = create_specific_linelists(ll; absent_chi_threshold=3.);
    hotkeys(; linelist=ll);
    apply_mask(ll; hydrogen=0.6);
    "or save";
    apply_mask(ll; save, hydrogen=0.6);

    help continuum_correction
    cc = continuum_correction(ll; interactive);
    save_local_continuum(cc);

    freeze_ignored();

    "plot the spectrum as a PDF; default ll";
    plot_spec(; xrange=1, width=35, ymax=1.06, ll=ll);

    "read synspec line list";
    ll = read_fort12("fort.12", 10.);

    "better continuum for UV spectra";
    freeze("c*y*");
    fit_cont_UV(-4, 5.);
    fit_cont_UV(-2.4, 4.5);
    fit_cont_UV(-1.4, 4.);

    "to exlude absent lines in UV (id, thres_lo, thres_hi)";
    ignore_thres(1, -3., 5.);

    `;
%}}}
  vmessage(helptext);
}

define fit_rvc()
  % Fit only radial velocity correction, and freeze it in ignored regions
{
  % Bail if there are no rv splines
  if(get_params("stellar(1).*yrv*")[0] == NULL)
  {
    vmessage("No action because rvcorr is deactivated. Set rvcorr = 1 and start a fresh fit.");
    return;
  }
  variable free_init = freeParameters;
  variable ndata = length([specs]);
  exclude(all_data);
  _for id(1, ndata, 1) % loop over datasets
  {
    include(id);
    % Freeze radial velocity correction points in ignored regions:
    % ===========================================
    % temp1 also includes rvc splines!
    variable temp1 = get_params("stellar(1).*"); freeze("stellar(1).*");
    variable free = freeParameters; freeze(free);
    variable temp2 = get_data_info(id).notice_list;
    thaw(sprintf("cspline(1).d%d_y*", id));
    thaw(sprintf("stellar(1).d%d_yrv*", id));
    ignore(id); notice_list(id, temp2); () = eval_counts(; fit_verbose=-1);
    variable temp3 = length(get_par(sprintf("stellar(1).d%d_xrv*",id)));
    % freeze rvc point 'i' when no noticed pixels are between anchorpoints 'i-1' and 'i+1'
    _for i(0, temp3-1, 1)
    {
      variable temp4 = get_par(sprintf("stellar(1).d%d_xrv%d",id,max([0,i-1])));
      variable temp5 = get_par(sprintf("stellar(1).d%d_xrv%d",id,min([i+1,temp3-1])));
      variable xcurr = get_par(sprintf("stellar(1).d%d_xrv%d",id,i));
      variable dhi = (temp5 - xcurr);
      variable dlo = (xcurr - temp4);
      temp4 = temp4 + dlo / 1.5;
      temp5 = temp5 - dhi / 1.5;
      % no noticed pixels between anchorpoints 'i-1' and 'i+1'
      if(length(where(temp4<get_data_counts(id).bin_lo[temp2]<temp5))==0)
      {
        variable temp6;
        foreach temp6 (["x","y"])
        {
          variable temp7 = get_par_info(sprintf("stellar(1).d%d_%srv%d",id,temp6,i));
          freeze(temp7.index);
          free = free[where(free!=temp7.index)];
          __uninitialize(&temp7);
        }
        __uninitialize(&temp6);
      }
      __uninitialize(&temp4); __uninitialize(&temp5);
    }
    __uninitialize(&temp2); __uninitialize(&temp3);
    set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
%    set_params(temp1);
    __uninitialize(&temp1);
  }
  %
  vmessage("> radial velocity correction summary");
  freeze("*");
  thaw("stellar(1).d*_*rv*");
  list_free;
  %
  freeze("*");
  thaw(free_init);
}

define derive_vrad_orbit(fp_times){
  if(len_comp!=2){
    vmessage("bailing: number of components != 2.");
    return NULL;
  }
  % First set qualifier 'dummy=5' in 'initialize_grid_fit_spectroscopy'!
  variable times = ascii_read_table(fp_times, [{"%F","hjd"}]);
  times = times.hjd;
  variable tmin = min(times);
  times -= tmin;
  vmessage(sprintf("shifting time by T0 = %.8f", tmin));
  variable fstr_c1, fstr_c2;
  _for id(1, len_sets, 1){
      fstr_c1 = sprintf("stellar(1).dummy_3 * sin(2*PI / stellar(1).dummy_2 * (%.4f - stellar(1).dummy_1)) + stellar(1).dummy_5", times[id-1]);
      fstr_c2 = sprintf("-stellar(1).dummy_3 / stellar(1).dummy_4 * sin(2*PI / stellar(1).dummy_2 * (%.4f - stellar(1).dummy_1)) + stellar(1).dummy_5", times[id-1]);
      set_par_fun(sprintf("stellar(1).d%d_c1_vrad", id), fstr_c1);
      set_par_fun(sprintf("stellar(1).d%d_c2_vrad", id), fstr_c2);
  }
  % dummy_1 -> T0
  % dummy_2 -> orbital period
  % set sensible limits for 'dummy' parameters;
  set_par("stellar(1).dummy_3", 0.5; min=0.005, max=1500); % orbital period (days)
  set_par("stellar(1).dummy_3", 10.; min=5, max=500); % K1
  set_par("stellar(1).dummy_4", 1.; min=0.1, max=10.); % mass ratio M_c2/M_c1
  set_par("stellar(1).dummy_5"; min=-600, max=600); % system velocity
}

define derive_vrad(){
  % derived vrad -> this is an approximation for e=0
  if(len_comp!=2){
    vmessage("bailing: number of components != 2.");
    return NULL;
  }
  variable M1 = 0.2;
  variable M2 = 0.7;
  variable q = M1/M2;
  variable fit_fun_old = get_fit_fun();
  if(string_match(fit_fun_old, "dummy")==0){
    % Define a do-nothing fit function with a single parameter
    add_slang_function("dummy", "q");
    fit_fun(fit_fun_old+"*dummy(1)");
    set_par("dummy(1).q", q; min=0.1, max=1.);
  }
  % loop over datasets
  _for id(1, len_sets, 1){
%    set_par_fun(sprintf("stellar(1).d%d_c2_vrad", id),
%                sprintf("-stellar(1).d%d_c1_vrad*%.3f", id, M1/M2));
    set_par_fun(sprintf("stellar(1).d%d_c2_vrad", id),
                sprintf("-stellar(1).d%d_c1_vrad*dummy(1).q", id));
  };
  eval_counts;
}

define fit_individual()
{
  % fit spectra one-by-one
  variable free = freeParameters; freeze(free); exclude(all_data);
  % loop over datasets
  _for id(1, len_sets, 1)
  {
    include(id);
    % thaw previously free parameters containing the string ".d$id_" in their name
    thaw(free[(where(array_map(Integer_Type, &string_match, array_map(String_Type, &get_struct_field, get_params(free), "name"),
              sprintf(".d%d_",id))!=0))]);
    set_fit_method("mpfit"); () = fit_counts; set_fit_method("powell"); () = fit_counts;
    exclude(id); freeze("*");
  };
  include(all_data); thaw(free);
  eval_counts;
}

define uncertainties_individual()
{
  % fit vrads + continuum
  % get uncertainties one-by-one
  variable free = freeParameters; freeze(free); exclude(all_data);
  _for id(1, len_sets, 1) % loop over datasets
  {
    include(id);
    % thaw previously free parameters containing the string ".d$id_" in their name
    thaw(free[(where(array_map(Integer_Type, &string_match, array_map(String_Type, &get_struct_field, get_params(free), "name"),
              sprintf(".d%d_",id))!=0))]);
    compute_uncertainties (; fp_save=sprintf("results/results_conf_d%d.fits", id));
    exclude(id); freeze("*");
  };
  include(all_data); thaw(free);
  eval_counts;
}

define get_teff_logg_confmap(){
  id = 1;
  i = 1;
  variable xpar = sprintf("stellar(1).d%d_c%d_teff",id,i);
  variable ypar = sprintf("stellar(1).d%d_c%d_logg",id,i);
  freeze("*");
  thaw("c*y*");
  thaw(xpar);
  thaw(ypar);
  variable conf_min, conf_max;
  (conf_min, conf_max) = conf_loop(freeParameters(; fit_fun_component="stellar"), 0, 1e-1; num_slaves=1);
  save_par("params/params"); save_par("params/params_5");
  variable stat; () = eval_counts(&stat; fit_verbose=-1);
  fits_write_binary_table("fitsfiles/results_conf.fits", "pvm_fit_pars-results",
   conf_loop_summary(freeParameters(; fit_fun_component="stellar"), conf_min, conf_max),
    struct{
      chi2 = stat.statistic,
      num_bins = stat.num_bins,
      n_var_pars = stat.num_variable_params,
      dof = stat.num_bins-stat.num_variable_params,
      chi2red = stat.statistic/(stat.num_bins-stat.num_variable_params)
          }
  );
  variable s = fits_read_table("fitsfiles/results_conf.fits");
  if(get_par_info(xpar).fun==NULL && get_par_info(xpar).tie==NULL && get_par_info(ypar).fun==NULL && get_par_info(ypar).tie==NULL)
  {
    variable xpar_i = where(s.name==xpar)[0];
    variable ypar_i = where(s.name==ypar)[0];
    variable xpar_r = 0.02*s.value[xpar_i]; % 1 per cent systematic uncertainty
    variable ypar_r = 0.1; % 0.04 dex systematic uncertainty
    variable xpar_n = 9;
    variable ypar_n = 9;
    variable xpar_lo = s.value[xpar_i] - sqrt((s.value[xpar_i]-s.conf_min[xpar_i])^2+xpar_r^2);
    variable xpar_hi = s.value[xpar_i] + sqrt((s.conf_max[xpar_i]-s.value[xpar_i])^2+xpar_r^2);
    variable ypar_lo = s.value[ypar_i] - sqrt((s.value[ypar_i]-s.conf_min[ypar_i])^2+ypar_r^2);
    variable ypar_hi = s.value[ypar_i] + sqrt((s.conf_max[ypar_i]-s.value[ypar_i])^2+ypar_r^2);
    % make sure that the spot of the best fit is sampled in confmap:
    (xpar_lo, xpar_hi, xpar_n) = conf_grid_bestfit(xpar_lo, xpar_hi, xpar_n, s.value[xpar_i]);
    (ypar_lo, ypar_hi, ypar_n) = conf_grid_bestfit(ypar_lo, ypar_hi, ypar_n, s.value[ypar_i]);
    % 'get_confmap' crashes if it tries to sample outside the allowed range
    xpar_lo = max([xpar_lo, get_par_info(xpar).min]);
    xpar_hi = min([xpar_hi, get_par_info(xpar).max]);
    ypar_lo = max([ypar_lo, get_par_info(ypar).min]);
    ypar_hi = min([ypar_hi, get_par_info(ypar).max]);
    % confidence maps:
    variable out = sprintf("d%d_c%d_%s_%s",id,i,strreplace(xpar,sprintf("stellar(1).d%d_c%d_",id,i),""),
                                                strreplace(ypar,sprintf("stellar(1).d%d_c%d_",id,i),""));
    variable temp = get_params("stellar(1).*");
    variable save_pars = Integer_Type[length(temp)];
    variable j;
    _for j(0,length(temp)-1,1){save_pars[j]=temp[j].index;}; __uninitialize(&temp);
    % compute confidence map
    variable confmap = get_confmap(xpar, xpar_lo, xpar_hi, xpar_n, ypar, ypar_lo, ypar_hi, ypar_n;
                       save=sprintf("fitsfiles/confmap_%s",out), flood, num_slaves=1, save_pars=save_pars);
    () = system(sprintf("gzip -f %sfitsfiles/confmap_%s.fits", wd, out));
    variable r = confmap_errors(sprintf("fitsfiles/confmap_%s.fits.gz",out), xpar_lo, xpar_hi, ypar_lo, ypar_hi;
                                stat_sys="fitsfiles/results_conf.fits", xsys=xpar_r, ysys=ypar_r);
    temp = get_par_info("stellar(1).*");
    r = struct_combine(r,struct{range_min=array_map(Double_Type,&get_struct_field,temp,"min"),
                                range_max=array_map(Double_Type,&get_struct_field,temp,"max")});
    __uninitialize(&temp);
    print_struct(sprintf("results/atmos_params_%s.txt",out), r);
    xfig_plot_teff_logg_confmap(sprintf("fitsfiles/confmap_%s.fits.gz",out), xpar_lo, xpar_hi, ypar_lo, ypar_hi);
  };
}
