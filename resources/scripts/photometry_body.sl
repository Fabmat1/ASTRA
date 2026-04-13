% Convert "SALT J" names to ra, dec in degrees
if(string_match(star, "SALT ?[Jj][0-9]+.*[-+][0-9]+.*"R)){
  variable jradec = coord_from_identifier(star);
  coordinates.ra = jradec.ra;
  coordinates.dec = jradec.dec;
}
% read "photometry_results.txt" or "photometry.dat" header
define read_photometry_header(filename) {
   variable fp = fopen(filename, "r");
   if (fp == NULL) error("Cannot open file");
   variable keys = {}, values = {}, line;
   while (-1 != fgets(&line, fp)) {
      if (line[0] != '#') break;
      line = strtrim(substr(line, 2, strlen(line)));
      line = strreplace(line, "=", " = ");
      variable tokens = strtok(line), i;
      _for i (0, length(tokens)-3, 3) {
         if (tokens[i+1] == "=") {
            list_append(keys, tokens[i]);
            list_append(values, tokens[i+2]);
         }
      }
   }
   () = fclose(fp);
   if(length(keys)==0) return NULL;
   variable header = @Struct_Type(list_to_array(keys));
   _for i (0, length(keys)-1, 1) {
      variable temp = strreplace(values[i], ";", "");
      variable data_types = union(array_map(DataType_Type, &_slang_guess_type, temp));
      if (length(where(data_types==String_Type))==0) {
         if (length(where(data_types==Double_Type or data_types==Float_Type or data_types==Complex_Type))>0)
            set_struct_field(header, keys[i], atof(temp));
         else
            set_struct_field(header, keys[i], atoi(temp));
      }
      else
         set_struct_field(header, keys[i], temp);
   }
   return header;
}
variable astrometry = NULL;
if(stat_file(basename+"photometry_results.txt")!=NULL){
  variable phdr = read_photometry_header(basename+"photometry_results.txt");
  if(phdr!=NULL and
   struct_field_exists(phdr, "parallax") and
   struct_field_exists(phdr, "parallax_error") and
   struct_field_exists(phdr, "RA") and
   struct_field_exists(phdr, "DEC")){
    variable fields = get_struct_field_names(phdr);
    variable hdr_lower = @Struct_Type(array_map(String_Type, &strlow, fields));
    foreach (fields) {
       variable field = ();
       set_struct_field(hdr_lower, strlow(field), get_struct_field(phdr, field));
    }
    astrometry = hdr_lower;
    variable good_astrometry = (astrometry!=NULL && isnan(astrometry.parallax)==0 && astrometry.parallax>0);
    coordinates.ra = astrometry.ra;
    coordinates.dec = astrometry.dec;
  }
}
variable temp;
if(coordinates.ra==NULL or coordinates.dec==NULL)
{
  temp = resolve_simbad(star);
  if(temp==NULL)
    throw DataError, sprintf("Data error: Object '%s' could not be resolved and no coordinates were provided either.", star);
  else % Object resolved with Sesame/Simbad
  {
    coordinates.ra = temp.ra;
    coordinates.dec = temp.dec;
    __uninitialize(&temp);
  }
}

if((fix_distance == NULL) and (astrometry==NULL)){
  variable astrometry_cols = "designation,parallax,parallax_error,"+
                             "ruwe,ipd_gof_harmonic_amplitude,ipd_frac_multi_peak,visibility_periods_used";
  astrometry = query_astrometry(coordinates.ra, coordinates.dec;
                                quality_warnings, corrected_values,
                                search_radius=4./60/60, columns=astrometry_cols);
  good_astrometry = (astrometry!=NULL && isnan(astrometry.parallax)==0 && astrometry.parallax>0);
}
else if(fix_distance!=NULL){
  astrometry = struct{parallax=1/fix_distance, parallax_error, ra, dec,
                      ruwe, ipd_gof_harmonic_amplitude, visibility_periods_used};
  astrometry.parallax_error = astrometry.parallax * fix_distance_err / fix_distance;
  astrometry.ruwe = 1.0;
  astrometry.ipd_gof_harmonic_amplitude = 0.0;
  astrometry.ra = coordinates.ra;
  astrometry.dec = coordinates.dec;
  astrometry.visibility_periods_used = 1;
  good_astrometry = 1;
}
if(good_astrometry) % get distance for 3d reddening map
{
  % remove negative parallaxes and parallaxes that are consistent with negative values
  if((astrometry.parallax>0)!=1) astrometry.parallax = _NaN;
  if((astrometry.parallax/astrometry.parallax_error>=1.)!=1) astrometry.parallax = _NaN;
  if(struct_field_exists(astrometry, "parallax_corr") && struct_field_exists(astrometry, "parallax_error_corr")){
    if((astrometry.parallax_corr>0)!=1) astrometry.parallax_corr = _NaN;
    if((astrometry.parallax_corr/astrometry.parallax_error_corr>=1.)!=1) astrometry.parallax_corr = _NaN;
    % use corrected parallax (Lindegren et al. 2020) and parallax_error (El-Badry, Rix, & Heintz 2021) if possible
    if(isnan(astrometry.parallax_corr)==0 && astrometry.parallax_corr>0 &&
       isnan(astrometry.parallax_error_corr)==0 && astrometry.parallax_error_corr>0){
      % add "parallax_offset" paramters for printing later
      astrometry = struct_combine(astrometry, struct{parallax_offset=-astrometry.parallax_corr+astrometry.parallax});
      % always use the corrected parallax
      astrometry.parallax = astrometry.parallax_corr;
      astrometry.parallax_error = astrometry.parallax_error_corr;
    }
  }
  good_astrometry = (astrometry!=NULL && isnan(astrometry.parallax)==0 && astrometry.parallax>0);
  variable gdist = 1.0e3 / astrometry.parallax;
}
else
  gdist = 0;
if(coordinates.ra!=NULL and coordinates.dec!=NULL) % Coordinates are provided
{
  if(astrometry!=NULL && star==NULL) % Get Gaia ID
    star = astrometry.designation;
  if(astrometry==NULL && star==NULL) % Use coordinates for name
    star = sprintf("%s %s", coordinates.ra, coordinates.dec);
}
%
% Query Stilism for distance - E(B-V) relation
variable gal_l, gal_b;
(gal_l, gal_b) = radec2lb(coordinates.ra, coordinates.dec);
% stilism is down
%variable rstilism = query_stilism_data(gal_l, gal_b);
variable rstilism = NULL;
%
% ----------------------------------------------
% Create new or read existing photometric table:
variable photo;
if(stat_file(basename+"photometry.dat")==NULL){
  photo = query_photometry(coordinates.ra, coordinates.dec;
                           IUE, MAST, search_radius=4./60/60);
  if(gdist>0)
    photo.interstellar_reddening = query_reddening(photo.get_ra(), photo.get_dec(); distance=gdist);
  else
    photo.interstellar_reddening = query_reddening(photo.get_ra(), photo.get_dec());
}
else{
  photo = photometric_table(star);
  photo.read(basename+"photometry.dat");
}
%
% option to add boxes from user-supplied spectrum
#ifeval 0
% read your flux-calibrated spectrum here (erg/cm^2/s/Angstrom).
variable cat_user = "CALSPEC";
variable l_user, f_user, ferr_user;
% (l_user, f_user) = vega_spectrum();
% ferr_user = f_user / 25.;
(l_user, f_user, ferr_user) = readcol("ofh329010_x1d.txt", 1, 2, 3);
%variable lmask = where(l_user>1150 & l_user<1728);
%l_user = l_user[lmask];
%f_user = f_user[lmask];
%ferr_user = ferr_user[lmask];
%variable s_user = fits_read_table("dustcor_star70.fits");
%l_user = s_user.wavelength;
%f_user = s_user.flux__uncorrected_;
%ferr_user = s_user.uncertainty__uncorrected_;
variable spec_user = struct{wavelength=l_user, flux=f_user, flux_error=ferr_user};
% define sensible boxes, in Angstrom
variable boxes_user = [[1000:3000:100],[3000:4500:250],
                       [4500:7000:500],[7000:15000:1000],
                       [15000:50000:5000]];
boxes_user = union(boxes_user);
variable b0 = array_map(String_Type, &string, boxes_user[[0:length(boxes_user)-2]]);
variable b1 = array_map(String_Type, &string, boxes_user[[1:length(boxes_user)-1]]);
boxes_user = b0 + "_" + b1;
photo = boxes_from_spectrum(photo, spec_user, cat_user; boxes=boxes_user);
#endif
%
% check photo
if(photo.photometric_entries==NULL)
  throw DataError, sprintf("Data Error: photometry.dat does not contain photometric entries.");
if((typeof(photo.photometric_entries.flag)==Array_Type)==0)
  throw DataError, sprintf("Data Error: Did not find enough magnitudes.");
if(length(where(photo.photometric_entries.flag==0))==0)
  throw DataError, sprintf("Data Error: All magnitudes are flagged.");
%
if(stilism_ebmv_rerun or (stat_file(basename+"photometry.dat")==NULL)){
  if((gdist>0) and (rstilism!=NULL)){
    if(stilism_ebmv_simple){
      % this method is superior to the MC approach or the simple one below
      % stilism is down
%      variable sd = query_stilism_distance(gal_l, gal_b, gdist);
      variable sd = NULL;
      if (sd!=NULL){
        photo.interstellar_reddening = struct_combine(photo.interstellar_reddening, "meanStilism", "stdStilism");
        photo.interstellar_reddening.meanStilism = sd.meanStilism;
        photo.interstellar_reddening.stdStilism = sd.stdStilism;
      }
%      variable rstilism_dist = stilism_interpol(rstilism; distance=1e3/astrometry.parallax,
%                                                distance_min=1e3/(astrometry.parallax+astrometry.parallax_error),
%                                                distance_max=1e3/(astrometry.parallax-astrometry.parallax_error));
%      photo.interstellar_reddening.meanStilism = rstilism_dist.reddening_best;
%      photo.interstellar_reddening.stdStilism = (rstilism_dist.reddening_best-rstilism_dist.reddening_best_min)/2 + (rstilism_dist.reddening_best_max-rstilism_dist.reddening_best)/2;
    }
    else{
      vmessage("Computing Stilism E(B-V) -");
      variable tstart = _ftime;
      variable distance_MC_for_stilism = 1.0e3 / (astrometry.parallax + grand(nint(10^3))*astrometry.parallax_error);
      photo.interstellar_reddening = struct_combine(photo.interstellar_reddening, "meanStilism", "stdStilism");
      variable rstilism_dist = stilism_interpol(rstilism; distance_MC=distance_MC_for_stilism);
      if(struct_field_exists(rstilism_dist, "reddening_MC")){
        temp = mode_and_HDI (rstilism_dist.reddening_MC);
        __uninitialize(&rstilism_dist);
        if(isnan(temp.mode)){
          temp.mode = temp.median;
          temp.HDI_lo = temp.quantile_lo;
          temp.HDI_hi = temp.quantile_hi;
        }
        photo.interstellar_reddening.meanStilism = temp.mode;
        photo.interstellar_reddening.stdStilism = ((temp.mode-temp.HDI_lo) + (temp.HDI_hi-temp.mode)) / 2.;
        __uninitialize(&temp);
      }
      vmessage(sprintf("- done in %.3f s", _ftime - tstart)); __uninitialize(&tstart);
    }
  }
}
if (stat_file(basename+"photometry.dat")==NULL)
  photo.write(basename+"photometry.dat");
% Assign a generic uncertainty to those measurements that do not have realistic uncertainties yet:
variable mask = where(photo.photometric_entries.flag==0 and
                      (photo.photometric_entries.uncertainty<1e-5 or
                       isnan(photo.photometric_entries.uncertainty)==1));
photo.photometric_entries.uncertainty[mask] = 0.025;
% ----------------------------------------------
% First guess for the color excess E_44m55 from reddening maps:
variable E_44m55 = NULL;
variable E_44m55_3d = NULL;
variable E_44m55_2d = NULL;
if(struct_field_exists(photo, "interstellar_reddening") &&
   photo.interstellar_reddening!=NULL &&
   length(where(par_full.name=="E_44m55"))==0)
{
  if(struct_field_exists(photo.interstellar_reddening, "meanStilism"))
    E_44m55_3d = photo.interstellar_reddening.meanStilism;
  if(struct_field_exists(photo.interstellar_reddening, "meanSandF"))
    E_44m55_2d = photo.interstellar_reddening.meanSandF;
  else if(struct_field_exists(photo.interstellar_reddening, "meanSFD"))
    E_44m55_2d = photo.interstellar_reddening.meanSFD;
  variable outside_stilism_range = 0;
  if(astrometry!=NULL && astrometry.parallax!=NULL &&
     astrometry.parallax/astrometry.parallax_error>2 &&
     astrometry.parallax<=0.5)
    outside_stilism_range = 1;
  % 1/0.91 is the approximate conversion from E(B-V) to E(44-55)
  if(outside_stilism_range==0 && E_44m55_3d!=NULL)
    E_44m55 = E_44m55_3d / 0.91;
  else
    E_44m55 = E_44m55_2d / 0.91;
  if(E_44m55!=NULL)
  {
    if(E_44m55>2) E_44m55 = 2.;
    par_full.name = ["E_44m55", par_full.name];
    par_full.value = [E_44m55, par_full.value];
    par_full.freeze = [0, par_full.freeze];
    par_full.min = [0, par_full.min];
    par_full.max = [5, par_full.max];
  }
}

variable i, l, f, chisqr_red = _NaN, norm_chi_red = 0;
% Fit with grids of synthetic SEDs
#ifeval 1
  variable ndummy = 0;
  if(good_astrometry) ndummy = 4;
  initialize_grid_fit_photometry(griddirectories; dummy=ndummy);
  if(good_astrometry){
    fit_fun("fit_theta_interext_atmparams");
    set_par("fit_theta_interext_atmparams(1).dummy_1", astrometry.parallax; min=0, max=200.);
    % derived radius
    set_par_fun("fit_theta_interext_atmparams(1).dummy_2",
                "10^fit_theta_interext_atmparams(1).logtheta/(2e-3*fit_theta_interext_atmparams(1).dummy_1)*4.4353565926280975e+07");
    % derived luminosity
    set_par_fun("fit_theta_interext_atmparams(1).dummy_3",
                "fit_theta_interext_atmparams(1).dummy_2^2*(fit_theta_interext_atmparams(1).c1_teff/5772.)^4");
    % derived mass
    set_par_fun("fit_theta_interext_atmparams(1).dummy_4",
                "10^fit_theta_interext_atmparams(1).c1_logg*fit_theta_interext_atmparams(1).dummy_2^2*3.6469715273112305e-05");
  }
  variable tfit_start = _ftime;
  variable p = photometric_fitting(photo.photometric_entries;
                apply_ZPO_corr=apply_ZPO_corr,
                conf_level=conf_level, fit_verbose=0, verbose=1, set_par_full=par_full,
                set_par=par, no_clean, chisqr_red=&chisqr_red, norm_chi_red=&norm_chi_red,
                remove_outliers=5);
  variable tfit_end = _ftime;
  vmessage(sprintf("- fit done in %.1fs", tfit_end-tfit_start));
#elseif % Provide SED and fit only 'logtheta', 'E_44m55', and 'R_55'
  (l,f) = readcol("SED.txt", 1, 2);
  variable p = photometric_fitting(l, f, photo.photometric_entries;
                                   conf_level=conf_level, fit_verbose=0, verbose=1,
                                   set_par_full=par_full, no_clean,
                                   chisqr_red=&chisqr_red, norm_chi_red=&norm_chi_red,
                                   remove_outliers=5);
#endif

% example of how to add a 'fake' companion after the fit
% surface ratio has to be set to 0 in 'par' at the top
#ifeval 0
variable fake_c2_sur_ratio = 30.;
par.value[where(par.name=="c2_sur_ratio")[0]] = fake_c2_sur_ratio;
p.value[where(p.name=="c2_sur_ratio")[0]] = fake_c2_sur_ratio;
set_par("*c2_sur*", fake_c2_sur_ratio, 1);
eval_counts;
#endif

% MCMC calculation
#ifeval 0
% usually: 1 - logtheta, 2 - E_44m55, 7 - teff; 8 - logg
%variable priors = struct{parlist=[where(p.name=="c1_teff")[0],
%                                  where(p.name=="c1_logg")[0]],
%                         priorlist=[struct{mean=27000, sigma=1000},
%                                    struct{mean=5.5, sigma=0.1}]};
variable nw = 50; % number of walkers, nw, per free parameter
variable nsim = 300; % number of iterations, "walker"-steps
variable num_slaves = 2;
emcee(nw, nsim;
  output = "emcee-chain.fits",
%  num_slaves=num_slaves,
%  priors=priors,
%  serial % use a single core
);
% to read
%variable nfreepar, freepar, chain_statistic, chain;
%(nw, nfreepar, freepar, chain_statistic, chain) = read_chain("emcee-chain.fits");
%variable nburn = nint(0.3 * nw * nsim * nfreepar);
%variable len_chain = length(chain[freepar[0]]);
%variable contour_levels = [[0.68268,0.954499736]];
%plot_xycontour(chain[0][[nburn:len_chain-1:1]],
%               chain[1][[nburn:len_chain-1:1]];
%               contour_levels=contour_levels,
%               name="emcee_12.pdf");
% variable acceptance_rate;
% acceptance_rate = read_chain("emcee-chain.fits"; frac_update);
% corner plot (does not work)
%variable matrix = distribution_matrix(chain);
%variable X = xfig_plot_distribution_matrix(matrix);
%X.render("matrix.pdf");
#endif

%
% Example of how to derive logg from teff, assuming HB nature and using the BaSTI tracks
define get_HB_full(pb, ph)
{
  % merge BaSTI and Han+ (2002) tracks; Han provides the connection to the HeMS
  variable track_ehb = evol_track(0.471, 0.0, 0.0; points=[pb], grid="basti_p030");
  variable track_bhb = evol_track(0.490, 0.0, 0.0; points=[pb], grid="basti_m130");
  variable track_rhb = evol_track(0.490, 0.0, 0.0; points=[pb], grid="basti_m030");
  variable mask_basti = where(19000. < 10^track_ehb.log_teff < 25000.);
  struct_filter(track_ehb, mask_basti);
  mask_basti = where(8000. < 10^track_bhb.log_teff <= 19000.);
  struct_filter(track_bhb, mask_basti);
  mask_basti = where(10^track_rhb.log_teff <= 8000.);
  struct_filter(track_rhb, mask_basti);
  variable han = evol_track(0, 0.0, 0.47; points=[ph], grid="han_Z0");
  han.mass += 0.47;
  variable field;
  foreach field (get_struct_field_names(han))
  {
    variable vals = [get_struct_field(han, field),
                     get_struct_field(track_ehb, field),
                     get_struct_field(track_bhb, field),
                     get_struct_field(track_rhb, field)];
    set_struct_field(track_ehb, field, vals);
  }
  return track_ehb;
}

%
variable isort, tx, ty;
% points=[120] -> ignore mass and compute "isochrone"; 120 - > intermediate age HB
%variable track = evol_track(0.525, 0.0, 0.000397; points=[120], grid="basti_m190");
variable track = get_HB_full(170, 10);
tx = 10^track.log_teff; ty = track.logg; isort = array_sort(tx); tx = tx[isort]; ty = ty[isort];
tx = [0., tx, 1.0e6]; ty = [ty[0], ty, ty[-1]]; % no extrapolation
public define get_logg_from_teff(teff){
  variable logg = interpol(teff, tx, ty);
  variable logg_grid = photometry_fit->grid_info.coverage[0].g;
  logg = min([max(logg_grid), logg]); logg = max([min(logg_grid), logg]);
  return logg;
}

variable tms = evol_track(1.0, 0, 0.0143*10^z_c2; points=[110], grid="basti_low");
variable tmsx, tms_R, tms_logg;
tmsx = 10^tms.log_teff; isort = array_sort(tmsx); tmsx = tmsx[isort];
tms_logg = tms.logg; tms_logg = tms_logg[isort];
tms_R = tms.radius; tms_R = tms_R[isort];
% no extrapolation
tmsx = [0., tmsx, 1.0e6];
tms_R = [tms_R[0], tms_R, tms_R[-1]];
tms_logg = [tms_logg[0], tms_logg, tms_logg[-1]];
public define get_sr_from_teff(teff){
  variable ms_radius = interpol(teff, tmsx, tms_R);
  variable ms_sr = (ms_radius / sdOB_radius)^2;
%  print(ms_sr);
  return ms_sr;
}
public define get_logg_from_teff_MS(teff){
  variable logg = interpol(teff, tmsx, tms_logg);
  variable logg_grid = photometry_fit->grid_info.coverage[1].g;
  logg = min([max(logg_grid), logg]); logg = max([min(logg_grid), logg]);
  return logg;
}

if(derive_logg){
  set_par_fun("fit_theta_interext_atmparams(1).c1_logg", "get_logg_from_teff(fit_theta_interext_atmparams(1).c1_teff)");
}
if(derive_sr){
  set_par_fun("fit_theta_interext_atmparams(1).c2_sur_ratio", "get_sr_from_teff(fit_theta_interext_atmparams(1).c2_teff)");
%  set_par_fun("fit_theta_interext_atmparams(1).c3_sur_ratio", "get_sr_from_teff(fit_theta_interext_atmparams(1).c3_teff)");
}
if(derive_logg_c2){
  set_par_fun("fit_theta_interext_atmparams(1).c2_logg", "get_logg_from_teff_MS(fit_theta_interext_atmparams(1).c2_teff)");
}
if(derive_logg or derive_sr or derive_logg_c2){
  p = photometric_fitting(photo.photometric_entries;
                apply_ZPO_corr=apply_ZPO_corr,
                conf_level=conf_level, fit_verbose=0, verbose=1, set_par_full=par_full,
                set_par=par, no_clean, chisqr_red=&chisqr_red, norm_chi_red=&norm_chi_red,
                remove_outliers=5);
}

%
variable nmag_good = length(where(photo.photometric_entries.flag==0));
%
% get reddening distance
%
if(rstilism!=NULL){
  if(stilism_distance_simple!=1)
    vmessage("Computing Stilism distance -");
  variable tstart = _ftime;
  variable ebmv_best = p.value[where(p.name=="E_44m55")][0];
  variable ebmv_min = p.conf_min[where(p.name=="E_44m55")][0];
  variable ebmv_max = p.conf_max[where(p.name=="E_44m55")][0];
  if(ebmv_min!=ebmv_min) ebmv_min = ebmv_best;
  if(ebmv_max!=ebmv_max) ebmv_max = ebmv_best;
  if(((ebmv_min!=ebmv_best) or (ebmv_min!=ebmv_best)) and (stilism_distance_simple!=1)){
    variable ebmv_perr = ebmv_min - ebmv_best;
    variable ebmv_nerr = ebmv_max - ebmv_best;
    variable MC_runs = nint(10^3);
    variable gpos = abs(grand(MC_runs/2))*ebmv_perr;
    variable gneg = abs(grand(MC_runs/2))*ebmv_nerr;
    variable gboth = [gneg,gpos];
    gboth = gboth [array_sort(gboth)];
    variable ebmv_MC = ebmv_best + gboth;
    % estimate uncertainty due to fit ebmv and Stilism ebmv
    rstilism = stilism_interpol(rstilism; reddening_MC=ebmv_MC);
  }
  else{
    if(stilism_distance_simple){
      % consider only the uncertainty due to Stilism ebmv
      rstilism = stilism_interpol(rstilism; reddening=ebmv_best, reddening_min=ebmv_min, reddening_max=ebmv_max);
    }
    else{
      % estimate only the uncertainty due to fit ebmv
      rstilism = stilism_interpol(rstilism; reddening=ebmv_best, MC_data);
    }
  }
  if(stilism_distance_simple!=1){
    if (struct_field_exists(rstilism, "distance_MC")){
      temp = mode_and_HDI(rstilism.distance_MC; p=0.68268);
      if(isnan(temp.mode)){
        temp.mode = temp.median;
        temp.HDI_lo = temp.quantile_lo;
        temp.HDI_hi = temp.quantile_hi;
      }
      temp.HDI_lo = temp.mode - sqrt((temp.HDI_lo-temp.mode)^2 + (temp.mode*0.1)^2);
      temp.HDI_hi = temp.mode + sqrt((temp.HDI_hi-temp.mode)^2 + (temp.mode*0.1)^2);
      % The mode tends to underestimate the distance due to the step-like E(B-V)-distance relation.
      % However, statistically, it is more appropriate.
      % Add the "reddening=ebmv_best" qualifier to stilism_interpol to use best-fit ebmv.
      if (struct_field_exists(rstilism, "distance_best")==0)
        rstilism = struct_combine(rstilism, struct{distance_best=temp.mode});
      rstilism = struct_combine(rstilism, struct{distance_best_min=temp.HDI_lo});
      rstilism = struct_combine(rstilism, struct{distance_best_max=temp.HDI_hi});
      if( (rstilism.distance_best_max==rstilism.distance_best) and (rstilism.distance_best_min!=rstilism.distance_best) )
        rstilism.distance_best_max = _Inf;
    }
  }
  if(stilism_distance_simple!=1)
    vmessage(sprintf("- done in %.3f s", _ftime - tstart)); __uninitialize(&tstart);
}
#ifeval 1
% Propagate uncertainties in prescribed parameters into logtheta
% (requires the "no_clean" qualifier in the function 'photometric_fitting' above):
if(string_match(get_fit_fun,"fit_theta_interext_atmparams")==1)
{
  variable params = get_params; % save best-fit parameters
  variable ind_logtheta = where(p.name=="logtheta")[0];
  variable logtheta = struct{ value = p.value[ind_logtheta],
                               minu = p.value[ind_logtheta]-p.conf_min[ind_logtheta],
                               plus = p.conf_max[ind_logtheta]-p.value[ind_logtheta] };
  variable ind_c1_teff = where(p.name=="c1_teff")[0];
  variable c1_teff = struct{ value = p.value[ind_c1_teff],
                              minu = p.value[ind_c1_teff]-p.conf_min[ind_c1_teff],
                              plus = p.conf_max[ind_c1_teff]-p.value[ind_c1_teff] };
  if(p.tex[ind_logtheta]!="\ldots"R)
  {
    variable field, tdiff, teff_diff;
    _for i(1, length(griddirectories), 1)
    {
      foreach field (["teff", "logg", "xi", "z", "HE", "sur_ratio", "E_44m55"])
      {
        variable temp1;
        if((string_match(field, "44m55") > 0) && i==1){
          % there may be multiple entries for E_44m55
          temp1 = [where(par_full.name==sprintf("%s",field))[-1]];
        }
        else
          temp1 = where(par_full.name==sprintf("c%d_%s",i,field));
        % prescribed parameter values, e.g., from spectroscopy
        if(length(temp1)>0 && par_full.freeze[temp1[0]]==1)
        {
          try
          {
            variable temp2;
            if((string_match(field, "44m55") > 0) && i==1)
              temp2 = where(p.name==sprintf("%s",field));
            else
              temp2 = where(p.name==sprintf("c%d_%s",i,field));
            %
            set_par(p.index[temp2[0]], par_full.min[temp1[0]]);
            () = fit_counts(; fit_verbose=-1);
            tdiff = logtheta.value - get_par(p.index[ind_logtheta]);
            if(tdiff>0)
              logtheta.minu = sqrt(logtheta.minu^2 + tdiff^2);
            else
              logtheta.plus = sqrt(logtheta.plus^2 + tdiff^2);
            %
            teff_diff = c1_teff.value - get_par(p.index[ind_c1_teff]);
            if(c1_teff.minu==c1_teff.minu && c1_teff.plus==c1_teff.plus){
              if(teff_diff>0)
                c1_teff.minu = sqrt(c1_teff.minu^2 + teff_diff^2);
              else
                c1_teff.plus = sqrt(c1_teff.plus^2 + teff_diff^2);
            }
            else{
              if(teff_diff>0)
                c1_teff.minu = abs(teff_diff);
              else
                c1_teff.plus = abs(teff_diff);
            }
            %
            set_par(p.index[temp2[0]], par_full.max[temp1[0]]);
            () = fit_counts(; fit_verbose=-1);
            tdiff = logtheta.value - get_par(p.index[ind_logtheta]);
            if(tdiff>0)
              logtheta.minu = sqrt(logtheta.minu^2 + tdiff^2);
            else
              logtheta.plus = sqrt(logtheta.plus^2 + tdiff^2);
            %
            teff_diff = c1_teff.value - get_par(p.index[ind_c1_teff]);
            if(c1_teff.minu==c1_teff.minu && c1_teff.plus==c1_teff.plus){
              if(teff_diff>0)
                c1_teff.minu = sqrt(c1_teff.minu^2 + teff_diff^2);
              else
                c1_teff.plus = sqrt(c1_teff.plus^2 + teff_diff^2);
            }
            else{
              if(teff_diff>0)
                c1_teff.minu = abs(teff_diff);
              else
                c1_teff.plus = abs(teff_diff);
            }
            %
            set_params(params); % restore best-fit parameters
          }
          catch AnyError:
          {
            vmessage(sprintf("Warning: prescribed error propagation failed for %s", par_full.name[temp1[0]]));
          }
        }
        __uninitialize(&temp1); __uninitialize(&temp2); __uninitialize(&tdiff);
      }
      __uninitialize(&field);
    }
    p.conf_min[ind_logtheta] = logtheta.value - logtheta.minu;
    p.conf_max[ind_logtheta] = logtheta.value + logtheta.plus;
    p.buf_below[ind_logtheta] = (p.conf_min[ind_logtheta]-p.min[ind_logtheta])/(p.max[ind_logtheta]-p.min[ind_logtheta]);
    p.buf_above[ind_logtheta] = (p.max[ind_logtheta]-p.conf_max[ind_logtheta])/(p.max[ind_logtheta]-p.min[ind_logtheta]);
    p.tex[ind_logtheta] = TeX_value_pm_error(logtheta.value, p.conf_min[ind_logtheta], p.conf_max[ind_logtheta], p.min[ind_logtheta], p.max[ind_logtheta]; sci=6);

    p.conf_min[ind_c1_teff] = c1_teff.value - c1_teff.minu;
    p.conf_max[ind_c1_teff] = c1_teff.value + c1_teff.plus;
    p.buf_below[ind_c1_teff] = (p.conf_min[ind_c1_teff]-p.min[ind_c1_teff])/(p.max[ind_c1_teff]-p.min[ind_c1_teff]);
    p.buf_above[ind_c1_teff] = (p.max[ind_c1_teff]-p.conf_max[ind_c1_teff])/(p.max[ind_c1_teff]-p.min[ind_c1_teff]);
    if(p.freeze[ind_c1_teff]==0)
      p.tex[ind_c1_teff] = TeX_value_pm_error(c1_teff.value, p.conf_min[ind_c1_teff], p.conf_max[ind_c1_teff], p.min[ind_c1_teff], p.max[ind_c1_teff]; sci=6);
  }
  __uninitialize(&params); __uninitialize(&ind_logtheta); __uninitialize(&logtheta); __uninitialize(&ind_c1_teff);
}
#endif

% Example of how to compute the HB model-based distance:
%
define get_dist_from_teff(teff, delta_teff_minu, delta_teff_plus,
                          logtheta, delta_logtheta_minu, delta_logtheta_plus)
{
  variable MC_runs = qualifier("MC_runs", nint(2e5));
  variable MC = struct{logtheta, teff, logg, M_Msun, R_Rsun, L_Lsun, sdistance};
  % set up teff
  MC.teff = grand(MC_runs);
  MC.teff[where(MC.teff<0)] *= delta_teff_minu;
  MC.teff[where(MC.teff>0)] *= delta_teff_plus;
  MC.teff += teff;
  % set up logtheta
  MC.logtheta = grand(MC_runs);
  MC.logtheta[where(MC.logtheta<0)] *= delta_logtheta_minu;
  MC.logtheta[where(MC.logtheta>0)] *= delta_logtheta_plus;
  MC.logtheta += logtheta;
  % variable track = evol_track(0.525, 0.0, 0.001572; points=[120], grid="basti_m130");
  % variable track = get_HB_full(1, 1); % ZAHB (HB age = 0; [Fe/H] ~ 0);
  variable track = get_HB_full(170, 10); % IAHB (YC = 0.5; [Fe/H] ~ 0);
  %
  variable tx = 10^track.log_teff;
  variable isort = array_sort(tx); tx = tx[isort]; tx = [0., tx, 1.0e6];
  variable tyr = track.radius[isort]; tyr = [tyr[0], tyr, tyr[-1]];
  tyr = log10(tyr);
  variable tyl = track.log_l[isort]; tyl = [tyl[0], tyl, tyl[-1]];
  variable tym = track.mass[isort]; tym = [tym[0], tym, tym[-1]];
  %
  MC.M_Msun = Double_Type[MC_runs];
  MC.R_Rsun = Double_Type[MC_runs];
  variable i;
  _for i(0, MC_runs-1)
  {
    MC.R_Rsun[i] = interpol(MC.teff[i], tx, tyr);
    MC.M_Msun[i] = interpol(MC.teff[i], tx, tym);
  }
  % add "systematic" uncerainty to the HB band
  variable Merr_sys = 0.05; % Msun
  variable M_offset = 0.0; % -0.03: 0.50 -> 0.47 Msun
  variable Rerr_sys = 0.10; % in log10
  MC.R_Rsun += grand(MC_runs) * Rerr_sys;
  MC.M_Msun += grand(MC_runs) * Merr_sys + M_offset;
  %
  variable Const_loggsun = log10(Const_GMsun_cgs / Const_Rsun_cgs^2);
  MC.logg = Const_loggsun + log10(MC.M_Msun) - 2*MC.R_Rsun;
  MC.R_Rsun = 10^MC.R_Rsun;
  MC.L_Lsun = MC.R_Rsun^2*(MC.teff/Const_Teffsun)^4;
  %
%  MC.sdistance = 2.*sqrt(MC.M_Msun*Const_GMsun_cgs*10^(-MC.logg))/10^MC.logtheta/Const_pc_cgs*1e-3;
  MC.sdistance = 2.*MC.R_Rsun*Const_Rsun_cgs/10^MC.logtheta/Const_pc_cgs*1e-3;
  return MC;
}

define clean_struct(s){
  variable field;
  foreach field(get_struct_field_names(s)){
    if(get_struct_field(s, field)==NULL){
      s = struct_drop_fields(s, [field]);
    }
    else if(length(get_struct_field(s, field))==0){
      s = struct_drop_fields(s, [field]);
    }
  }
  return s;
}

% -------------
% Save results:
variable fp = fopen(basename+"photometry_results.txt", "w");
variable res_header;
if(photo.ra!=NULL and photo.dec!=NULL){
  res_header = sprintf("# RA = %.8f; DEC = %.8f; norm_chi_red = %.4f; chisqr_reduced = %.3f;\n",
                       photo.ra, photo.dec, norm_chi_red, chisqr_red);
}
else{
  res_header = sprintf("# RA = %.8f; DEC = %.8f; norm_chi_red = %.4f; chisqr_reduced = %.3f;\n",
                       0.0, 0.0, norm_chi_red, chisqr_red);
}
variable grid_short = strchop(griddirectories[0],'/',1)[-3]+"/"+strchop(griddirectories[0],'/',1)[-2];
res_header += sprintf("# nmag_good = %d; grid = %s;\n", nmag_good, grid_short);
if(astrometry!=NULL)
{
  res_header += "# ";
  variable hfields_gaia = ["ruwe", "ipd_gof_harmonic_amplitude",
                           "visibility_periods_used",
                           "parallax", "parallax_error"];
  variable hfield;
  foreach hfield (hfields_gaia)
  {
    if(struct_field_exists(astrometry, hfield) && get_struct_field(astrometry, hfield)!=NULL)
      res_header += sprintf("%s = %.3g; ", hfield, get_struct_field(astrometry, hfield));
  }
  res_header += "\n";
}
if(photo.get_reddening()!=NULL){
  variable redd = photo.get_reddening();
  res_header += "# ";
  variable hfields_redd = ["meanSFD", "meanSandF", "meanStilism"];
  foreach hfield (hfields_redd)
  {
    if(struct_field_exists(redd, hfield) && get_struct_field(redd, hfield)!=NULL)
      res_header += sprintf("%s = %.3g; ", hfield, get_struct_field(redd, hfield));
  }
  res_header += "\n";
}
() = fputs(res_header, fp);
print_struct(fp, p);
() = fclose(fp);
% ------------

#ifeval 1
% Print results to a PDF file:
%variable astrometry = query_astrometry(photo.ra, photo.dec; corrected_values, search_radius=0.02, columns="parallax,parallax_error,ruwe");
variable confidence, table_head_line = "Object: "+star+ " & "R;
if(conf_level==-1 or conf_level==0)
{
  confidence = 0.68268;
  table_head_line += "68\%% confidence interval"R;
}
else if(conf_level==1)
{
  confidence = 0.9;
  table_head_line += "90\%% confidence interval"R;
  if(isnan(astrometry.parallax)==0)
    astrometry.parallax_error *= 1.645; % conversion from 68% to 90% confidence limit
}
else if(conf_level==2)
{
  confidence = 0.99;
  table_head_line += "99\%% confidence interval"R;
  if(isnan(astrometry.parallax)==0)
    astrometry.parallax_error *= 2.576; % conversion from 68% to 99% confidence limit
}
else
  table_head_line += " Value";
variable fp = fopen(basename+"photometry_results.tex", "w");
() = fprintf(fp, "\documentclass{standalone}"R+"\n"+"\usepackage{amsmath,txfonts,color}"R+"\n"+"\usepackage[colorlinks=true,urlcolor=black]{hyperref}"R+"\n"+"\begin{document}"R+"\n"+"\renewcommand{\arraystretch}{1.2}"R+"\n");
() = fprintf(fp, "\begin{tabular}{lr}"R+"\n"+"\hline\hline"R+"\n"+table_head_line+"\\"R+"\n"+"\hline"R+"\n");
if(photo.get_reddening()!=NULL){
  if(struct_field_exists(photo.interstellar_reddening, "meanSFD") && photo.get_reddening().meanSFD!=NULL){
  () = fprintf(fp, "Color excess $E(B-V)$ from \href{https://ui.adsabs.harvard.edu/abs/1998ApJ...500..525S/abstract}{SFD (1998)} & %s\,mag \\"R+"\n",
               TeX_value_pm_error(photo.get_reddening().meanSFD,photo.get_reddening().meanSFD-photo.get_reddening().stdSFD,photo.get_reddening().meanSFD+photo.get_reddening().stdSFD));
  }
  if(struct_field_exists(photo.interstellar_reddening, "meanSandF") && photo.get_reddening().meanSandF!=NULL){
  () = fprintf(fp, "Color excess $E(B-V)$ from \href{https://ui.adsabs.harvard.edu/abs/2011ApJ...737..103S/abstract}{S\&F (2011)} & %s\,mag \\"R+"\n",
               TeX_value_pm_error(photo.get_reddening().meanSandF,photo.get_reddening().meanSandF-photo.get_reddening().stdSandF,photo.get_reddening().meanSandF+photo.get_reddening().stdSandF));
  }
  if(struct_field_exists(photo.interstellar_reddening, "meanStilism") && photo.get_reddening().meanStilism!=NULL){
    () = fprintf(fp, "Color excess $E(B-V)$ from \href{https://stilism.obspm.fr/}{Stilism} \href{https://ui.adsabs.harvard.edu/abs/2017A%%26A...606A..65C/abstract}{(Capitanio+ 2017)} & %s\,mag \\"R+"\n",
                 TeX_value_pm_error(photo.get_reddening().meanStilism,photo.get_reddening().meanStilism-photo.get_reddening().stdStilism,photo.get_reddening().meanStilism+photo.get_reddening().stdStilism));
  }
  if(struct_field_exists(photo.interstellar_reddening, "bestBayestar") && photo.get_reddening().bestBayestar!=NULL){
    () = fprintf(fp, "Color excess $E(B-V)$ from \href{http://argonaut.skymaps.info/}{Bayestar15} \href{https://ui.adsabs.harvard.edu/abs/2015ApJ...810...25G/abstract}{(Green+ 2015)} & %s\,mag \\"R+"\n",
                 TeX_value_pm_error(photo.get_reddening().bestBayestar,photo.get_reddening().bestBayestar-photo.get_reddening().stdBayestar,photo.get_reddening().bestBayestar+photo.get_reddening().stdBayestar));
  }
}
if(rstilism!=NULL)
{
  () = fprintf(fp, "Distance from Stilism and $E(44-55)$ & %s\,pc \\"R+"\n",
               TeX_value_pm_error(rstilism.distance_best, rstilism.distance_best_min, rstilism.distance_best_max; sci=3));

}
() = fprintf(fp, "\hline"R+"\n");
variable j, temp, s = struct{ name_fit=["E_44m55", "R_55", "logtheta"],
                              name_table=["Color excess $E(44-55)$"R,
                                          "Extinction parameter $R(55)$"R,
                                          "Angular diameter $\log(\Theta\,\mathrm{(rad)})$"R],
                              unit=["\,mag"R, "", ""] };
_for j(0, length(s.name_fit)-1, 1)
{
  temp = p.tex[where(p.name==s.name_fit[j])][0];
  if(temp!="\ldots"R)
    () = fprintf(fp, "%s & %s%s \\"R+"\n", s.name_table[j], temp, s.unit[j]);
  else
  {
    temp = where(p.name==s.name_fit[j])[0];
    if(p.freeze[temp]==0)
    {
      if(p.value[temp]==p.min[temp] or p.value[temp]==p.max[temp])
        () = fprintf(fp, "%s & \color{red}{$%g$}%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
      else
        () = fprintf(fp, "%s & $%g$%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
    }
    else
      () = fprintf(fp, "%s (fixed) & $%g$%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
  }
  __uninitialize(&temp);
}
variable MC_c1 = NULL;
variable MC_c2 = NULL;
if(good_astrometry){
  if(struct_field_exists(astrometry, "parallax_offset") && astrometry.parallax_offset!=NULL){
    % ZPO is the parallax zero point offset following Lindegren+ (2021) (which was subtracted from the EDR3 parallaxes).
    () = fprintf(fp, "Parallax $\varpi$ ({\it Gaia}, $\text{RUWE}=%.2f$, $\text{ZPO}=%.2g$\,mas) & %s\,mas \\"R+"\n",
                 astrometry.ruwe, astrometry.parallax_offset, TeX_value_pm_error(round_conf(astrometry.parallax, astrometry.parallax_error)));
  }
  else{
    () = fprintf(fp, "Parallax $\varpi$ ({\it Gaia}, $\text{RUWE}=%.2f$) & %s\,mas \\"R+"\n",
                 astrometry.ruwe, TeX_value_pm_error(round_conf(astrometry.parallax, astrometry.parallax_error)));
  }
  variable MC_runs = nMC; % number of Monte Carlo trials
  variable MC = struct{ parallax, logtheta, R_Rsun };
  if(astrometry.parallax > 0)
  {
    MC.parallax = astrometry.parallax + grand(MC_runs)*astrometry.parallax_error;
    temp = mode_and_HDI(1e3/MC.parallax; p=confidence);
    () = fprintf(fp, "Distance $d$ ({\it Gaia}, mode) & %s\,pc \\"R+"\n",
                 TeX_value_pm_error(temp.mode, temp.HDI_lo, temp.HDI_hi; sci=3));
    () = fprintf(fp, "Distance $d$ ({\it Gaia}, median) & %s\,pc \\"R+"\n",
                 TeX_value_pm_error(temp.median, temp.quantile_lo, temp.quantile_hi; sci=3));
  }
}
if(string_match(get_fit_fun,"fit_theta_interext_atmparams")==0)
{
  if(good_astrometry)
  {
    variable ind_logtheta = where(p.name=="logtheta")[0];
    variable delta_logtheta_plus = 0, delta_logtheta_minu = 0;
    if(conf_level!=-1 && p.freeze[ind_logtheta]==0) % logtheta is a free parameter
    {
      delta_logtheta_plus = p.conf_max[ind_logtheta] - p.value[ind_logtheta];
      delta_logtheta_minu = p.value[ind_logtheta] - p.conf_min[ind_logtheta];
    }
    % Use a Monte Carlo method to propagate uncertainties, which is more reliable than linear error propagation when
    % errors are large and which yields similar results when errors are small:
    MC.logtheta = grand(MC_runs); MC.logtheta[where(MC.logtheta<0)] *= delta_logtheta_minu;
    MC.logtheta[where(MC.logtheta>0)] *= delta_logtheta_plus; MC.logtheta += p.value[ind_logtheta];
    % Theta = 2*R/d = 2*R*parallax -> R = Theta/(2*parallax)
    MC.R_Rsun = 10^MC.logtheta/(2e-3*MC.parallax)*Const_pc_cgs/Const_Rsun_cgs;
    % remove unphysical Monte Carlo trials
    struct_filter(MC, where(MC.parallax>1e-4));
    temp = mode_and_HDI(MC.R_Rsun; p=confidence);
    () = fprintf(fp, "Radius $R = \Theta/(2\varpi)$ & %S\,$R_\odot$ \\"R+"\n", TeX_value_pm_error(temp.mode, temp.HDI_lo, temp.HDI_hi; sci=3));
    __uninitialize(&temp);
  }
}
else
{
  s = struct{ name_fit=["teff", "logg", "xi", "z", "HE", "sur_ratio"],
              name_table=["Effective temperature $T_{\mathrm{eff}}$"R,
                          "Surface gravity $\log (g\,\mathrm{(cm\,s^{-2})})$"R,
                          "Microturbulence $\xi$"R,
                          "Metallicity $z$"R,
                          "Helium abundance $\log(n(\textnormal{He}))$"R,
                          "Surface ratio $A_\mathrm{eff}/A_{\mathrm{eff,}1}$"R],
              unit=["\,K"R, ""R, "\,km\,s$^{-1}$"R, "\,dex"R, ""R, ""R] };
  variable pstellar_all = Struct_Type[length(griddirectories)];
  _for i(1, length(griddirectories), 1)
  {
    if(length(griddirectories)>1)
      () = fprintf(fp, "\hline"R+"\n"+"\multicolumn{2}{l}{Component %d:} \\"R+"\n", i);
    _for j(0, length(s.name_fit)-1, 1)
    {
      if(i!=1 or s.name_fit[j]!="sur_ratio") % do not print the surface ratio of the first component because it is fixed to 1
      {
	temp = p.tex[where(p.name==sprintf("c%d_%s",i,s.name_fit[j]))][0];
	if(temp!="\ldots"R)
	  () = fprintf(fp, "%s & %s%s \\"R+"\n", s.name_table[j], temp, s.unit[j]);
	else
	{
	  temp = where(par_full.name==sprintf("c%d_%s",i,s.name_fit[j]));
	  if(length(temp)==1 && par_full.freeze[temp[0]]==1) % prescribed parameter values, e.g., from spectroscopy
	    () = fprintf(fp, "%s (prescribed) & %s%s \\"R+"\n", s.name_table[j], TeX_value_pm_error(par_full.value[temp[0]], par_full.min[temp[0]], par_full.max[temp[0]]; sci=6), s.unit[j]);
	  else
	  {
	    temp = where(p.name==sprintf("c%d_%s",i,s.name_fit[j]))[0];
	    if(p.freeze[temp]==0)
	    {
	      if(p.value[temp]==p.min[temp] or p.value[temp]==p.max[temp])
		() = fprintf(fp, "%s & \color{red}{$%g$}%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
	      else
		() = fprintf(fp, "%s & $%g$%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
	    }
	    else
	      () = fprintf(fp, "%s (fixed) & $%g$%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
	  }
	  __uninitialize(&temp);
	}
	__uninitialize(&temp);
      }
    }
    if((mass_can>0) or (R1>0) or good_astrometry)
    {
      variable ind_logtheta = where(p.name=="logtheta")[0];
      variable ind_teff = where(p.name==sprintf("c%d_teff",i))[0];
      variable ind_logg = where(p.name==sprintf("c%d_logg",i))[0];
      variable ind_sur_ratio = where(p.name==sprintf("c%d_sur_ratio",i))[0];
      variable delta_logtheta_plus = 0, delta_logtheta_minu = 0;
      variable delta_teff_plus = 0, delta_teff_minu = 0;
      variable delta_logg_plus = 0, delta_logg_minu = 0;
      variable delta_sur_ratio_plus = 0, delta_sur_ratio_minu = 0;
      if(conf_level!=-1)
      {
      	if(p.freeze[ind_logtheta]==0) % logtheta is a free parameter
      	{
      	  delta_logtheta_plus = p.conf_max[ind_logtheta] - p.value[ind_logtheta];
	        delta_logtheta_minu = p.value[ind_logtheta] - p.conf_min[ind_logtheta];
        }
        temp = where(par_full.name==sprintf("c%d_teff",i));
        if(p.freeze[ind_teff]==0) % teff is a free parameter
        {
          delta_teff_plus = p.conf_max[ind_teff] - p.value[ind_teff];
          delta_teff_minu = p.value[ind_teff] - p.conf_min[ind_teff];
        }
        else if(length(temp)==1 && par_full.freeze[temp[0]]==1) % teff is a prescribed parameter
        {
          delta_teff_plus = p.max[ind_teff] - p.value[ind_teff];
          delta_teff_minu = p.value[ind_teff] - p.min[ind_teff];
        }
        temp = where(par_full.name==sprintf("c%d_logg",i));
        if(p.freeze[ind_logg]==0) % logg is a free parameter
        {
          delta_logg_plus = p.conf_max[ind_logg] - p.value[ind_logg];
          delta_logg_minu = p.value[ind_logg] - p.conf_min[ind_logg];
        }
        else if(length(temp)==1 && par_full.freeze[temp[0]]==1) % logg is a prescribed parameter
        {
          delta_logg_plus = p.max[ind_logg] - p.value[ind_logg];
          delta_logg_minu = p.value[ind_logg] - p.min[ind_logg];
        }
        temp = where(par_full.name==sprintf("c%d_sur_ratio",i));
        if(p.freeze[ind_sur_ratio]==0) % sur_ratio is a free parameter
        {
          delta_sur_ratio_plus = p.conf_max[ind_sur_ratio] - p.value[ind_sur_ratio];
          delta_sur_ratio_minu = p.value[ind_sur_ratio] - p.conf_min[ind_sur_ratio];
        }
        else if(length(temp)==1 && par_full.freeze[temp[0]]==1) % sur_ratio is a prescribed parameter
        {
          delta_sur_ratio_plus = p.max[ind_sur_ratio] - p.value[ind_sur_ratio];
          delta_sur_ratio_minu = p.value[ind_sur_ratio] - p.min[ind_sur_ratio];
        }
	__uninitialize(&temp);
      }
      % Use a Monte Carlo method to propagate uncertainties, which is more reliable than linear error propagation
      % when errors are large and which yields similar results when errors are small:
      variable MC_runs = nMC; % number of Monte Carlo trials
      variable MC = struct{ parallax, logtheta, sur_ratio, R_Rsun, logg, M_Msun, teff, L_Lsun, vgrav, vesc, sdistance};
      MC.logtheta = grand(MC_runs);
      MC.logtheta[where(MC.logtheta<0)] *= delta_logtheta_minu;
      MC.logtheta[where(MC.logtheta>0)] *= delta_logtheta_plus;
      MC.logtheta += p.value[ind_logtheta];
      if (R1>0)
      {
        variable MC_R1 = R1 + grand(MC_runs)*R1_err;
        MC.parallax = 5e2 * 10^MC.logtheta / MC_R1 / Const_Rsun_cgs * Const_pc_cgs;
        temp = mode_and_HDI(MC.parallax; p=confidence);
        () = fprintf(fp, "Parallax $\varpi$ (LC) (mode) & %s\,mas \\"R+"\n",
                     TeX_value_pm_error(temp.mode, temp.HDI_lo, temp.HDI_hi; sci=3));
        () = fprintf(fp, "\phantom{Parallax $\varpi$ (LC)} (median) & %s\,mas \\"R+"\n",
                     TeX_value_pm_error(temp.median, temp.quantile_lo, temp.quantile_hi; sci=3));
      }
      else
      {
        if(good_astrometry)
          MC.parallax = astrometry.parallax + grand(MC_runs)*astrometry.parallax_error;
        else
          MC.parallax = NULL;
      }
      MC.sur_ratio = grand(MC_runs); MC.sur_ratio[where(MC.sur_ratio<0)] *= delta_sur_ratio_minu; MC.sur_ratio[where(MC.sur_ratio>0)] *= delta_sur_ratio_plus;
      MC.sur_ratio += p.value[ind_sur_ratio];
      MC.logg = grand(MC_runs); MC.logg[where(MC.logg<0)] *= delta_logg_minu; MC.logg[where(MC.logg>0)] *= delta_logg_plus; MC.logg += p.value[ind_logg];
      MC.teff = grand(MC_runs); MC.teff[where(MC.teff<0)] *= delta_teff_minu; MC.teff[where(MC.teff>0)] *= delta_teff_plus; MC.teff += p.value[ind_teff];
      if(mass_can>0){
        % spectr. distance
        % theta = 2*R/d -> d = 2*R/theta = 2*sqrt(G*M/g)/10^logtheta in kpc, distances for fixes canonical mass
        variable mass_can_MC = mass_can + grand(MC_runs)*delta_mass_can;
        MC.sdistance = 2.*sqrt(mass_can_MC*Const_GMsun_cgs*10^(-MC.logg))/10^(MC.logtheta)/Const_pc_cgs*1e-3;
        temp = mode_and_HDI(MC.sdistance; p=confidence);
        vmessage(sprintf("dspec = %.3f -%.3f + %.3f kpc", temp.mode, temp.mode-temp.HDI_lo, temp.HDI_hi-temp.mode));
        () = fprintf(fp, "Spec. distance  $d_\mathrm{spec}$ ($M$\,=\,%S\,$M_\odot$, mode) & %S\,$\mathrm{kpc}$ \\"R+"\n",
                     TeX_value_pm_error(mass_can, mass_can-delta_mass_can, mass_can+delta_mass_can; sci=3),
                     TeX_value_pm_error(temp.mode, temp.HDI_lo, temp.HDI_hi; sci=3));
        () = fprintf(fp, "Spec. distance  $d_\mathrm{spec}$ ($M$\,=\,%S\,$M_\odot$, median) & %S\,$\mathrm{kpc}$ \\"R+"\n",
                     TeX_value_pm_error(mass_can, mass_can-delta_mass_can, mass_can+delta_mass_can; sci=3),
                     TeX_value_pm_error(temp.median, temp.quantile_lo, temp.quantile_hi; sci=3));
        if(typeof(MC.parallax)!=Array_Type) MC.parallax = 1/MC.sdistance;
      }
      % -------------
      % Save stellar parameters to photometry_results_stellar.txt:
      variable pstellar = struct{name, value, conf_min, conf_max, median_value, median_conf_min, median_conf_max};
      variable cstr = sprintf("c%d", i);
      pstellar.name = [cstr+"_R", cstr+"_M", cstr+"_L"];
      pstellar.value = _NaN*Double_Type[3];
      pstellar.conf_min = _NaN*Double_Type[3];
      pstellar.conf_max = _NaN*Double_Type[3];
      pstellar.median_value = _NaN*Double_Type[3];
      pstellar.median_conf_min = _NaN*Double_Type[3];
      pstellar.median_conf_max = _NaN*Double_Type[3];
      % ------------
      % Assuming HB nature -> distance, R/L/M; the HB star must be the first component
      if(hb_distance and i==1)
      {
        vmessage("Estimating distance from Theta and Teff by assuming HB nature -");
        variable hb_pname = [cstr+"_R_HB", cstr+"_M_HB", cstr+"_L_HB", cstr+"_logg_HB", "distance_HB"];
        pstellar.name = [pstellar.name, hb_pname];
        foreach field (get_struct_field_names(pstellar))
        {
          if(field!="name")
            set_struct_field(pstellar, field, [get_struct_field(pstellar, field), _NaN*Double_Type[5]]);
        }
        variable tstart = _ftime;
        variable teff = p.value[ind_teff];
        %variable delta_teff_minu = 1000.; variable delta_teff_plus = 1000.;
        variable logtheta = p.value[ind_logtheta];
        %variable delta_logtheta_minu = 0.05; variable delta_logtheta_plus = 0.05;
        variable MC_hb = get_dist_from_teff(teff, delta_teff_minu, delta_teff_plus,
                                            logtheta, delta_logtheta_minu, delta_logtheta_plus;
                                            MC_runs=MC_runs);
        variable tstr = ["Radius $R_\mathrm{HB}$ (median, interpol) & %S\,$R_\odot$ \\"R+"\n",
                         "Mass $M_\mathrm{HB}$ (median, interpol) & %S\,$M_\odot$ \\"R+"\n",
                         "Luminosity $L_\mathrm{HB} = (R/R_\odot)^2(T_\mathrm{eff}/T_{\mathrm{eff},\odot})^4$ (median) & %S\,$L_\odot$ \\"R+"\n",
                         "Surface gravity $\log g_\mathrm{HB} = \log g_\odot + \log M/M_\odot - 2 \log R/R_\odot$ (median) & %S \\"R+"\n",
                         "Distance $d_\mathrm{HB} = 2R/\Theta$ (median) & %S\,$\mathrm{kpc}$ \\"R+"\n"];
        variable hb_pkey = ["R_Rsun", "M_Msun", "L_Lsun", "logg", "sdistance"];
        () = fprintf(fp, "\hline"R+"\n");
        variable k;
        _for k (0, length(tstr)-1, 1)
        {
          temp = mode_and_HDI(get_struct_field(MC_hb, hb_pkey[k]); p=confidence);
          variable pidx = k + 3;
          pstellar.value[pidx] = temp.mode; pstellar.conf_min[pidx] = temp.HDI_lo; pstellar.conf_max[pidx] = temp.HDI_hi;
          pstellar.median_value[pidx]=temp.median; pstellar.median_conf_min[pidx]=temp.quantile_lo; pstellar.median_conf_max[pidx]=temp.quantile_hi;
          () = fprintf(fp, tstr[k], TeX_value_pm_error(temp.median, temp.quantile_lo, temp.quantile_hi; sci=3));
        }
        () = fprintf(fp, "\hline"R+"\n");
        % if there is no parallax, use this distance
        if(typeof(MC.parallax)!=Array_Type) MC.parallax = 1/MC_hb.sdistance;
        __uninitialize(&MC_hb);
        vmessage(sprintf("- done in %.1f s", _ftime - tstart)); __uninitialize(&tstart);
      }
      if(typeof(MC.parallax)==Array_Type)
      {
        MC.R_Rsun = 10^MC.logtheta/(2e-3*MC.parallax)*Const_pc_cgs/Const_Rsun_cgs; % Theta = 2*R/d = 2*R*parallax -> R = Theta/(2*parallax)
        % MC.R_Rsun is so far radius of primary component R_1 -> radius of component 'i' is MC.R_Rsun times
        % the square root of the surface ratio, which is (R_i/R_1)^2
        MC.R_Rsun *= sqrt(MC.sur_ratio);
        % g = G*M/R^2 -> M = 10^logg*R^2/G
        MC.M_Msun = 10^MC.logg*MC.R_Rsun^2*Const_Rsun_cgs^2/Const_GMsun_cgs;
        % L/L_sun = (R/Rsun)^2*(Teff/Const_Teffsun)^4
        MC.L_Lsun = (MC.R_Rsun)^2*(MC.teff/Const_Teffsun)^4;
        % v_grav = GM/(R*c)
        MC.vgrav = MC.M_Msun*Const_GMsun_cgs/(MC.R_Rsun*Const_Rsun_cgs*Const_c)/1e5;
        MC.vesc = sqrt(2. * 10^MC.logg * MC.R_Rsun * Const_Rsun_cgs)/1e5;
        % remove unphysical Monte Carlo trials
        struct_filter(MC, where(MC.parallax>1e-4 and MC.sur_ratio>0));
 %       struct_filter(MC, where(MC.L_Lsun>0 and MC.M_Msun>0 and MC.R_Rsun>0));
        variable mask_temp = where(MC.R_Rsun>0);
        if(length(mask_temp)>0) MC.R_Rsun = MC.R_Rsun[mask_temp];
        else MC.R_Rsun = Double_Type[1] * _NaN;
        mask_temp = where(MC.L_Lsun>0);
        if(length(mask_temp)>0) MC.L_Lsun = MC.L_Lsun[mask_temp];
        else MC.L_Lsun = Double_Type[1] * _NaN;
        mask_temp = where(MC.M_Msun>0);
        if(length(mask_temp)>0) MC.M_Msun = MC.M_Msun[mask_temp];
        else MC.M_Msun = Double_Type[1] * _NaN;
        mask_temp = where(MC.vgrav>0);
        if(length(mask_temp)>0) MC.vgrav = MC.vgrav[mask_temp];
        else MC.vgrav = Double_Type[1] * _NaN;
        mask_temp = where(MC.vesc>0);
        if(length(mask_temp)>0) MC.vesc = MC.vesc[mask_temp];
        else MC.vesc = Double_Type[1] * _NaN;
        temp = mode_and_HDI(MC.R_Rsun; p=confidence);
        () = fprintf(fp, "Radius $R = %s\Theta/(2\varpi)$ (mode) & %S\,$R_\odot$ \\"R+"\n", i==1 ? "" : "(A_\mathrm{eff}/A_{\mathrm{eff,}1})^{1/2}"R,
                     TeX_value_pm_error(temp.mode, temp.HDI_lo, temp.HDI_hi; sci=3));
        pstellar.value[0] = temp.mode; pstellar.conf_min[0] = temp.HDI_lo; pstellar.conf_max[0] = temp.HDI_hi;
        pstellar.median_value[0]=temp.median; pstellar.median_conf_min[0]=temp.quantile_lo; pstellar.median_conf_max[0]=temp.quantile_hi;
        () = fprintf(fp, "\phantom{Radius $R = %s\Theta/(2\varpi)$ }(median) & %S\,$R_\odot$ \\"R+"\n", i==1 ? "" : "(A_\mathrm{eff}/A_{\mathrm{eff,}1})^{1/2}"R,
                     TeX_value_pm_error(temp.median, temp.quantile_lo, temp.quantile_hi; sci=3));
        temp = mode_and_HDI(MC.M_Msun; p=confidence);
        () = fprintf(fp, "Mass $M = g R^2/G$ (mode) & %S\,$M_\odot$ \\"R+"\n",
                     TeX_value_pm_error(temp.mode, temp.HDI_lo, temp.HDI_hi; sci=3));
        pstellar.value[1] = temp.mode; pstellar.conf_min[1] = temp.HDI_lo; pstellar.conf_max[1] = temp.HDI_hi;
        pstellar.median_value[1]=temp.median; pstellar.median_conf_min[1]=temp.quantile_lo; pstellar.median_conf_max[1]=temp.quantile_hi;
        () = fprintf(fp, "\phantom{Mass $M = g R^2/G$ }(median) & %S\,$M_\odot$ \\"R+"\n",
                     TeX_value_pm_error(temp.median, temp.quantile_lo, temp.quantile_hi; sci=3));
        temp = mode_and_HDI(MC.L_Lsun; p=confidence);
        () = fprintf(fp, "Luminosity $L = (R/R_\odot)^2(T_\mathrm{eff}/T_{\mathrm{eff},\odot})^4$ (mode) & %S\,$L_\odot$ \\"R+"\n",
                     TeX_value_pm_error(temp.mode, temp.HDI_lo, temp.HDI_hi; sci=3));
        pstellar.value[2] = temp.mode; pstellar.conf_min[2] = temp.HDI_lo; pstellar.conf_max[2] = temp.HDI_hi;
        pstellar.median_value[2]=temp.median; pstellar.median_conf_min[2]=temp.quantile_lo; pstellar.median_conf_max[2]=temp.quantile_hi;
        () = fprintf(fp, "\phantom{Luminosity $L/L_\odot = (R/R_\odot)^2(T_\mathrm{eff}/T_{\mathrm{eff},\odot})^4$ }(median) & %S \\"R+"\n",
                     TeX_value_pm_error(temp.median, temp.quantile_lo, temp.quantile_hi; sci=3));
        temp = mode_and_HDI(MC.vgrav; p=confidence);
        () = fprintf(fp, "Gravitational redshift $\varv_\mathrm{grav} = GM/(Rc)$ & %s\,km\,s${}^{-1}$ \\"R+"\n",
                     TeX_value_pm_error(temp.mode, temp.HDI_lo, temp.HDI_hi; sci=3));
        temp = mode_and_HDI(MC.vesc; p=confidence);
        () = fprintf(fp, "Escape velocity $\varv_\mathrm{esc} = \sqrt{2gR}$ & %s\,km\,s${}^{-1}$ \\"R+"\n",
                     TeX_value_pm_error(temp.mode, temp.HDI_lo, temp.HDI_hi; sci=3));
      }
      __uninitialize(&temp);
      % ------------
      if(pstellar!=NULL)
      {
        variable fp2 = fopen(sprintf("%sphotometry_results_stellar_%s.txt", basename, cstr), "w");
        print_struct(fp2, pstellar);
        () = fclose(fp2);
        pstellar_all[i-1] = pstellar;
      }
      % ------------
      if(save_MC and i==1)
        MC_c1 = MC;
      else if(save_MC and i==2)
        MC_c2 = MC;
    }
  }
  if(get_par(get_fit_fun+".bb_teff")!=0 and get_par(get_fit_fun+".bb_sur_ratio")!=0)
  {
    () = fprintf(fp, "\hline"R+"\n"+"\multicolumn{2}{l}{Blackbody component:} \\"R+"\n");
    s = struct{ name_fit=["bb_teff", "bb_sur_ratio"],
                name_table=["Blackbody temperature $T_{\mathrm{bb}}$"R,
                            "Blackbody surface ratio $A_\mathrm{eff,bb}/A_{\mathrm{eff"R + (length(griddirectories)>1 ? ",}1}$"R : "}}$"R)],
                unit=["\,K"R, ""R] };
    _for j(0, length(s.name_fit)-1, 1)
    {
      temp = p.tex[where(p.name==s.name_fit[j])][0];
      if(temp!="\ldots"R)
        () = fprintf(fp, "%s & %s%s \\"R+"\n", s.name_table[j], temp, s.unit[j]);
      else
      {
        temp = where(p.name==s.name_fit[j])[0];
	if(p.freeze[temp]==0)
	{
	  if(p.value[temp]==p.min[temp] or p.value[temp]==p.max[temp])
	    () = fprintf(fp, "%s & \color{red}{$%g$}%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
	  else
	    () = fprintf(fp, "%s & $%g$%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
	}
        else
          () = fprintf(fp, "%s (fixed) & $%g$%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
      }
      __uninitialize(&temp);
    }
  }
  if(get_par(get_fit_fun+".bb2_teff")!=0 and get_par(get_fit_fun+".bb2_sur_ratio")!=0)
  {
    () = fprintf(fp, "\hline"R+"\n"+"\multicolumn{2}{l}{Blackbody component 2:} \\"R+"\n");
    s = struct{ name_fit=["bb2_teff", "bb2_sur_ratio"],
                name_table=["Blackbody temperature $T_{\mathrm{bb}}$"R,
                            "Blackbody surface ratio $A_\mathrm{eff,bb}/A_{\mathrm{eff"R + (length(griddirectories)>1 ? ",}1}$"R : "}}$"R)],
                unit=["\,K"R, ""R] };
    _for j(0, length(s.name_fit)-1, 1)
    {
      temp = p.tex[where(p.name==s.name_fit[j])][0];
      if(temp!="\ldots"R)
        () = fprintf(fp, "%s & %s%s \\"R+"\n", s.name_table[j], temp, s.unit[j]);
      else
      {
        temp = where(p.name==s.name_fit[j])[0];
    if(p.freeze[temp]==0)
    {
      if(p.value[temp]==p.min[temp] or p.value[temp]==p.max[temp])
        () = fprintf(fp, "%s & \color{red}{$%g$}%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
      else
        () = fprintf(fp, "%s & $%g$%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
    }
        else
          () = fprintf(fp, "%s (fixed) & $%g$%s \\"R+"\n", s.name_table[j], p.value[temp], s.unit[j]);
      }
      __uninitialize(&temp);
    }
  }
}
__uninitialize(&temp);
() = fprintf(fp, "\hline"R+"\n");
() = fprintf(fp, "Generic excess noise $\delta_\textnormal{excess}$ & $%.3f$\,mag \\"R+"\n", norm_chi_red);
() = fprintf(fp, "Reduced $\chi^2$ at the best fit & $%.2f$ \\"R+"\n", chisqr_red);
() = fprintf(fp, "\hline"R+"\n"+"\end{tabular}"R+"\n"+"\end{document}"R+"\n");
() = fclose(fp);
() = system(sprintf("pdflatex -halt-on-error -file-line-error %sphotometry_results.tex | grep 'error' --color=always; rm %sphotometry_results.aux %sphotometry_results.log %sphotometry_results.out;", basename, basename, basename, basename));
#endif

% ---------
% Plot SED:
% variable res_offset = _Inf, res_slope = _Inf;  % to have default resolution of processed ATLAS12 SEDs, which is R = 1000
% variable res_offset = 0, res_slope = 1./0.2; % to have similar resolution as high-resolution IUE spectra
variable res_offset = 0, res_slope = 1./6.0; % to have similar resolution as low-resolution IUE spectra
variable xmin = 1100, xmax = 59000; % -> up to W2; for W3 and W4: 180000
if(typeof(l)==Undefined_Type) % Grids of synthetic SEDs were fitted
{
  variable len = length(get_par(get_fit_fun+".c*teff"));
  variable metals = Struct_Type[len]; _for i(0,len-1,1){metals[i]=struct{metal=["ATLAS12"]};};
  (l,f) = syn_spec(get_par(get_fit_fun+".c*teff"),
                   get_par(get_fit_fun+".c*logg"),
                   get_par(get_fit_fun+".c*xi"),
                   get_par(get_fit_fun+".c*_z"),
                   get_par(get_fit_fun+".c*HE"), photometry_fit->grid_info;
        		   sur_ratio=get_par(get_fit_fun+".c*sur_ratio"),
                   metals=metals,
                   res_offset=res_offset, res_slope=res_slope,
                   xmin=xmin-10, xmax=xmax+10);
  if(get_par(get_fit_fun+".bb_teff")!=0 and get_par(get_fit_fun+".bb_sur_ratio")!=0)
  {
    variable l_bb, f_bb;
    (l_bb, f_bb) = blackbody_spectrum(get_par(get_fit_fun+".bb_teff"), l; N=1000);
    f += interpol(l, l_bb, get_par(get_fit_fun+".bb_sur_ratio")*f_bb);
  }
  if(get_par(get_fit_fun+".bb2_teff")!=0 and get_par(get_fit_fun+".bb2_sur_ratio")!=0)
  {
    variable l_bb2, f_bb2;
    (l_bb2, f_bb2) = blackbody_spectrum(get_par(get_fit_fun+".bb2_teff"), l; N=1000);
    f += interpol(l, l_bb2, get_par(get_fit_fun+".bb2_sur_ratio")*f_bb2);
  }
}
else % SED was provided
{
  variable ind = where(xmin-10 < l < xmax+10);
  (l,f) = convolve_syn(l[ind], f[ind], struct{vsini=0}; res_offset=res_offset, res_slope=res_slope);
}
% update the uncertainties to have the correct ones for the figure created with the function 'xfig_photometry'
if(norm_chi_red!=0) photo.photometric_entries.uncertainty = sqrt(photo.photometric_entries.uncertainty^2+norm_chi_red^2);
if(write_model)
{
  variable mout = magnitudes_to_flux(l, f, photo.photometric_entries;
                                     theta=10^get_par(get_fit_fun+".logtheta"),
                                     E_44m55=get_par(get_fit_fun+".E_44m55"), R_55=get_par(get_fit_fun+".R_55"),
                                     errbar, flagged);
  fp = fopen(sprintf("%sphotometry_fit_mag.txt", basename), "w");
  print_struct(fp, mout.mag);
  () = fclose(fp);
  if(length(mout.col.diff)>0)
  {
    fp = fopen(sprintf("%sphotometry_fit_col.txt", basename), "w");
    print_struct(fp, mout.col);
    () = fclose(fp);
  }
}
variable sf = interstellar_extinction(l, get_par(get_fit_fun+".E_44m55"), get_par(get_fit_fun+".R_55")) *
            (.5*10^get_par(get_fit_fun+".logtheta"))^2;
if(write_model)
{
  % ism extinction and angular diameter scaling
  variable sout = struct{l=l, f=f*sf};
}
%
% predict magnitudes from model; E(44-55) is alreay applied. Useful for ETCs.
variable predict_mag = 0;
if(predict_mag)
{
  variable fpred = f * sf;
  variable mpred = photometric_magnitudes(l, fpred);
  print_struct(mpred);
  % for the STIS/COS ETC
  writecol("model.dat", l, fpred);
}
%
variable factor, exponent = 3;
variable fig = xfig_photometry(l, f, photo.photometric_entries;
                colored, errbar, filter_width, theta=10^get_par(get_fit_fun+".logtheta"),
                E_44m55=get_par(get_fit_fun+".E_44m55"), R_55=get_par(get_fit_fun+".R_55"),
                verbose, xmin=xmin, xmax=xmax-1, nomultiplot, factor=&factor, chi,
%                flagged,
                exponent=exponent);
% --- to adjust font sizes ---
%                width=15,
%                height=10,
%                ylabel_size="\normalsize"R,
%                ylabel_2_size="\normalsize"R,
%                yaxis=struct{depth=0, tic_depth=0, ticlabels2=0, ticlabels_confine=1, ticlabel_size="\small"R},
%                xaxis_2=struct{depth=0, tic_depth=0, ticlabels2=0, ticlabel_size="\small"R, log},
%                yaxis_2=struct{depth=0, tic_depth=0, ticlabels2=0, ticlabels_confine=1, ticlabel_size="\small"R},
%                xaxis_3=struct{depth=0, tic_depth=0, ticlabels2=0, ticlabel_size="\small"R},
%                yaxis_3=struct{depth=0, tic_depth=0, ticlabel_size="\small"R},

% in case xfig leads to issues and sltikz is avaiable:
%variable fig = tikz_photometry(l, f, photo.photometric_entries; colored, errbar, filter_width, theta=10^get_par(get_fit_fun+".logtheta"), E_44m55=get_par(get_fit_fun+".E_44m55"), R_55=get_par(get_fit_fun+".R_55"), verbose=1,
%                   xmin=xmin, xmax=xmax-1, nomultiplot, factor=&factor, chi, exponent=exponent);
%
%
#ifeval 1
% Plot individual components if possible:
if(__is_initialized(&len)==1 && (len>1 or ((get_par(get_fit_fun+".bb_teff")!=0 and get_par(get_fit_fun+".bb_sur_ratio")!=0) or
                                           (get_par(get_fit_fun+".bb_teff")!=0 and get_par(get_fit_fun+".bb_sur_ratio")!=0))))
{
  variable lt, ft;
  if(get_par(get_fit_fun+".bb_teff")!=0 and get_par(get_fit_fun+".bb_sur_ratio")!=0)
  {
    (lt, ft) = blackbody_spectrum(get_par(get_fit_fun+".bb_teff"), l; N=0);
    if(write_model)
      sout = struct_combine(sout, struct{f_b=interpol(l, lt, ft)*sf});
    variable sf_b = interstellar_extinction(lt, get_par(get_fit_fun+".E_44m55"), get_par(get_fit_fun+".R_55")) *
                    (0.5*10^get_par(get_fit_fun+".logtheta"))^2;
    fig[0].plot(lt, get_par(get_fit_fun+".bb_sur_ratio") * ft * sf_b * lt^exponent / 10^factor;
                depth=9, line=0, width=1, color="pink3");
  }
  if(get_par(get_fit_fun+".bb2_teff")!=0 and get_par(get_fit_fun+".bb2_sur_ratio")!=0)
  {
    (lt, ft) = blackbody_spectrum(get_par(get_fit_fun+".bb2_teff"), l; N=0);
    if(write_model)
      sout = struct_combine(sout, struct{f_b=interpol(l, lt, ft)*sf});
    sf_b = interstellar_extinction(lt, get_par(get_fit_fun+".E_44m55"), get_par(get_fit_fun+".R_55")) *
                    (0.5*10^get_par(get_fit_fun+".logtheta"))^2;
    fig[0].plot(lt, get_par(get_fit_fun+".bb2_sur_ratio") * ft * sf_b * lt^exponent / 10^factor;
                depth=9, line=0, width=1, color="pink2");
  }
  _for i(0,len-1,1)
  {
    variable colour = "black";
    if(i==0) colour = "skyblue";
    if(i==1) colour = "pink3";
    variable len = length(get_par(get_fit_fun+".c*teff"));
    variable metals = Struct_Type[len], j; _for j(0,len-1,1){metals[j]=struct{metal=["ATLAS12"]};};
    variable sur_ratio = 0+Double_Type[len]; sur_ratio[i] = get_par(get_fit_fun+sprintf(".c%d_sur_ratio",i+1));
    (lt,ft) = syn_spec(get_par(get_fit_fun+".c*teff"),
                       get_par(get_fit_fun+".c*logg"),
                       get_par(get_fit_fun+".c*xi"),
                       get_par(get_fit_fun+".c*_z"),
                       get_par(get_fit_fun+".c*HE"),
                       photometry_fit->grid_info;
        		       sur_ratio=sur_ratio, metals=metals,
                       res_offset=res_offset, res_slope=res_slope,
                       xmin=xmin-10, xmax=xmax+10);
    if(write_model)
    {
      if(i==0)
        sout = struct_combine(sout, struct{f_c1=interpol(l, lt, ft)*sf});
      else if(i==1)
        sout = struct_combine(sout, struct{f_c2=interpol(l, lt, ft)*sf});
    }
    variable sf_c = interstellar_extinction(lt, get_par(get_fit_fun+".E_44m55"), get_par(get_fit_fun+".R_55")) *
                    (0.5*10^get_par(get_fit_fun+".logtheta"))^2;
    fig[0].plot(lt, ft * sf_c * lt^exponent / 10^factor; depth=10+i, line=0, width=1, color=colour);
    variable predict_mag_comp = 0;
    if(predict_mag_comp)
    {
      mpred = photometric_magnitudes(l, ft*sf_c);
%      print_struct(mpred);
      variable sp_cm = sprintf(basename+"mpred_c%d.txt", i+1);
      variable fp_cm = fopen(sp_cm, "w");
      print_struct(fp_cm, mpred);
      () = fclose(fp_cm);
      vmessage(sprintf("> saved model AB mag to %s", sp_cm));
    }
  }
}
#endif
%
if(write_model)
{
  fp = fopen(sprintf("%sphotometry_fit.txt", basename), "w");
  print_struct(fp, sout);
  () = fclose(fp);
}
% overplot user-supplied spectrum
#ifeval 0
variable l_user_plot = wavegrid_const_res(l_user[0], l_user[-1], 500.);
variable f_user_plot = rebin_wavegrid(l_user_plot, l_user, f_user);
fig[0].plot(l_user_plot,
            f_user_plot*l_user_plot^exponent/10^factor;
            width=1, depth=4, color="violet");
#endif
%
#ifeval 1
% Example of how to overplot a representative IUE spectrum:
variable box;
foreach box (union(photo.photometric_entries.passband[where(photo.photometric_entries.system=="box" and
                                                            photo.photometric_entries.flag==0)]))
{
  variable ind = where(photo.photometric_entries.passband==box and photo.photometric_entries.flag==0);
  _for i(0, length(ind)-1, 1)
  {
    variable IUE_file = "";
    % magnitude stems from a single IUE spectrum
    if(string_match(photo.photometric_entries.VizieR_catalog[ind[i]], "VI/110/inescat")!=0)
      IUE_file = "IUE/"+strreplace(photo.photometric_entries.VizieR_catalog[ind[i]], "_VI/110/inescat", ".FITS.gz");
    else if(photo.photometric_entries.VizieR_catalog[ind[i]]=="Average")
    {
      % several IUE spectra were averaged -> plot that spectrum that represents the average best
      variable temp = where(photo.photometric_entries.passband==box and photo.photometric_entries.flag==2);
      if(length(temp)>0)
      {
        % index of IUE spectrum whose magnitude is closest to the average
        temp = temp[where_min(abs(photo.photometric_entries.magnitude[ind[i]]-photo.photometric_entries.magnitude[temp]))[0]];
        IUE_file = "IUE/"+strreplace(photo.photometric_entries.VizieR_catalog[temp], "_VI/110/inescat", ".FITS.gz");
      }
    }
    if(stat_file(IUE_file)!=NULL)
    {
      variable IUE = fits_read_table(IUE_file);
      % remove bad pixels, see https://archive.stsci.edu/iue/manual/dacguide/node60.html
      struct_filter(IUE, where(IUE.flux>0 and IUE.quality==0));
      fig[0].plot(IUE.wavelength, IUE.flux*IUE.wavelength^exponent/10^factor; width=1, depth=4, color="violet");
    }
  }
}
#endif
%
if(length(fig)==2) xfig_new_compound(fig[0],fig[1]).render(basename+"photometry_SED.pdf");
else xfig_new_compound(fig[0],fig[1],fig[2]).render(basename+"photometry_SED.pdf");
% in case xfig leads to issues and sltikz is avaiable:
%if(length(fig)==2) tikz_new_compound(fig[0],fig[1]).render(basename+"photometry_SED.pdf");
%else tikz_new_compound(fig[0],fig[1],fig[2]).render(basename+"photometry_SED.pdf");
% ---------

#ifeval 0
% Example of how to print results to a PDF file in a form that is more appropriate for collecting results from many objects:
variable files = glob("*photometry_results.txt");
variable s = Struct_Type[length(files)];
variable i;
_for i(0, length(files)-1,1){
  variable name_collect = strreplace(strreplace(files[i],"photometry_results_spec.txt",""),"_"," ");
  % 'thres_sur_ratio' to only consider c1 if c2_sur_ratio < thres_sur_ratio
  s[i] = photometry_collect_results(name_collect, files[i]; thres_sur_ratio=0.1);
};
% 'remove_excess_fields' to remove fields that are not always present
% 'fill_excess_fields' to instead fill them with _NaN if not present
s = merge_struct_arrays(s; fill_excess_fields);
t = photometry_TeX_table(s; digit_RUWE=-2, digit_norm_chi_red=-3);
variable fp = fopen("photometry_collected_results.tex","w");
() = fprintf(fp, "\documentclass{standalone}"R+"\n"+"\usepackage{amsmath,txfonts,color}"R+"\n"+"\begin{document}"R+"\n"+"\renewcommand{\arraystretch}{1.2}"R+"\n"+"\setlength{\tabcolsep}{0.25em}"R+"\n");
() = fprintf(fp, "\begin{tabular}{l"R + multiple_string(length(get_struct_field_names(t))-1, "r") + "}\n"+"\hline\hline"R+"\n");
%() = fprintf(fp, "Object & & \multicolumn{2}{c}{$\log(\Theta)$} & & \multicolumn{2}{c}{$E(44-55)$} & & \multicolumn{2}{c}{$R(55)$} & & \multicolumn{2}{c}{$T_{\textnormal{eff}}$} & & \multicolumn{2}{c}{$\log(g)$}"R);
%() = fprintf(fp, " & & \multicolumn{2}{c}{$\xi$} & & \multicolumn{2}{c}{$z$} & & \multicolumn{2}{c}{$\log(n(\textnormal{He}))$} & & \multicolumn{2}{c}{$\varpi$} & & RUWE & & \multicolumn{2}{c}{$R_{\star}$}"R);
%() = fprintf(fp, " & & \multicolumn{2}{c}{$M$} & & \multicolumn{2}{c}{$\log(L/L_{\odot})$} & & $\delta_\textnormal{excess}$ \\ "R+"\n");
%() = fprintf(fp, "\cline{3-4} \cline{6-7} \cline{9-10} \cline{12-13} \cline{15-16} \cline{18-19} \cline{21-22} \cline{24-25} \cline{27-28} \cline{30-30} \cline{32-33} \cline{35-36} \cline{38-39} \cline{41-41}"R+"\n");
%() = fprintf(fp, "& & \multicolumn{2}{c}{(rad)} & & \multicolumn{2}{c}{(mag)} & & & & & \multicolumn{2}{c}{(K)} & & \multicolumn{2}{c}{(cgs)} & & \multicolumn{2}{c}{(km\,s$^{-1}$)} & & "R);
%() = fprintf(fp, "\multicolumn{2}{c}{(dex)} & & & & & \multicolumn{2}{c}{(mas)} & & & & \multicolumn{2}{c}{$(R_{\odot})$} & & \multicolumn{2}{c}{$(M_{\odot})$} & & & & & (mag) \\"R+"\n"+"\hline"R+"\n");
print_struct(fp, t; sep=" & ", final=" \\"R, nohead);
() = fprintf(fp, "\hline"R+"\n"+"\end{tabular}"R+"\n"+"\end{document}"R+"\n");
() = fclose(fp);
() = system("pdflatex -halt-on-error -file-line-error photometry_collected_results.tex | grep 'error' --color=always; rm photometry_collected_results.aux photometry_collected_results.log");
#endif

#ifeval 0
% Example of how to compute the spectrophotometric distance:
variable mass = 4.5; % mass in solar masses
variable delta_mass = 0.5; % mass uncertainty in solar masses
variable logg = 4.0; % get_par(get_fit_fun+".c1_logg"); % logg in cgs units
variable delta_logg = 0.1; % logg uncertainty in cgs units
variable ind = where(p.name=="logtheta")[0];
variable delta_logtheta_minu = 0, delta_logtheta_plus = 0;
if(conf_level==-1)
{
  delta_logtheta_plus = p.conf_max[ind] - p.value[ind];
  delta_logtheta_minu = p.value[ind] - p.conf_min[ind];
}
% Use a Monte Carlo method to propagate uncertainties, which is more reliable than linear error propagation
% when errors are large and which yields similar results when errors are small:
variable MC_runs = nMC; % number of Monte Carlo trials
variable logtheta = grand(MC_runs);
logtheta[where(logtheta<0)] *= delta_logtheta_minu;
logtheta[where(logtheta>0)] *= delta_logtheta_plus;
logtheta += p.value[ind];
% theta = 2*R/d -> d = 2*R/theta = 2*sqrt(G*M/g)/10^logtheta
variable distance = 2.*sqrt( (mass+grand(MC_runs)*delta_mass)*Const_GMsun_cgs*10^(-(logg+grand(MC_runs)*delta_logg)) )/10^logtheta/Const_pc_cgs;
temp = mode_and_HDI(distance; p=confidence);
% factor=1e-3 to convert from pc to kpc
vmessage("Distance d: %s\,kpc"R, TeX_value_pm_error(temp.mode, temp.HDI_lo, temp.HDI_hi; factor=1e-3));
__uninitialize(&temp);
#endif

#ifeval 1
% Save results as a .fits file
fp = fits_open_file(basename+"SED_results.fits", "c");
variable header = struct{ra=photo.ra, dec=photo.dec,
                         norm_chi_red=norm_chi_red,
                         chisqr_red=chisqr_red,
                         nmag_good=nmag_good,
                         grid_short=grid_short};
if(astrometry!=NULL)
{
  variable header_gaia = @Struct_Type(hfields_gaia);
  foreach hfield (hfields_gaia)
    if(isnan(get_struct_field(astrometry, hfield))==0){
      set_struct_field(header_gaia, hfield, get_struct_field(astrometry, hfield));;
    }
  header = struct_combine(header, header_gaia);
}
if(photo.get_reddening()!=NULL)
{
  variable header_redd = @Struct_Type(hfields_redd);
  foreach hfield (hfields_redd){
    if(struct_field_exists(redd, hfield))
      set_struct_field(header_redd, hfield, get_struct_field(redd, hfield));;
  }
  header = struct_combine(header, header_redd);
}
% write header to primary extension
fits_write_image_hdu(fp, NULL, Integer_Type[0], header, NULL);
% fit parameters
fits_write_binary_table(fp, "params_fit", p);
if(__is_initialized(&pstellar_all) && length(pstellar_all)>0)
{
  _for i(0, length(pstellar_all)-1)
  {
    variable extname = sprintf("stellar_c%d", i+1);
    grid_short = strchop(griddirectories[i],'/',1)[-3]+"/"+strchop(griddirectories[i],'/',1)[-2];
    header = struct{grid=grid_short};
    fits_write_binary_table(fp, extname, pstellar_all[i], header);
  }
}
if(write_model)
{
  % save observed magnitudes
  fits_write_binary_table(fp, "filters", mout.mag);
  if(length(mout.col.diff)>0)
    fits_write_binary_table(fp, "colours", mout.col);
  % save model spectrum
  fits_write_binary_table(fp, "spectrum_fit", sout);
}
% save IUE spectrum
if(__is_initialized(&IUE) && length(IUE.wavelength)>0)
{
  header = struct{file=IUE_file};
  variable table = struct{wavelength=IUE.wavelength,
                          flux=IUE.flux,
                          error=IUE.sigma};
  fits_write_binary_table(fp, "IUE", table, header);
}
% save MC arrays for stellar parameters to properly
% carry over uncertainties, e. g. for mass distributions
if(save_MC && MC_c1!=NULL){
  % only keep a few fields to avoid large files
  variable to_keep = ["R_Rsun", "L_Lsun", "M_Msun"];
  MC_c1 = struct_drop_fields(MC_c1, [to_keep]; keep);
  % remove empty fields
  MC_c1 = clean_struct(MC_c1);
  if(length(get_struct_field_names(MC_c1))>0)
    fits_write_binary_table(fp, "MC_c1", MC_c1);
  if(MC_c2!=NULL){
    MC_c2 = struct_drop_fields(MC_c2, [to_keep]; keep);
    MC_c2 = clean_struct(MC_c2);
    if(length(get_struct_field_names(MC_c2))>0)
      fits_write_binary_table(fp, "MC_c2", MC_c2);
  }
}
% read in python as:
% from astropy.io import fits; from astropy.table import Table;
% with fits.open(fp) as hdul: df = Table(hdul[1].data).to_pandas()
fits_close_file(fp);
#endif

#ifeval 0
% Example of how to compute and plot a two dimensional confidence map:
variable conf_map;
variable sname_p1 = "c1_teff";
variable sname_p2 = "E_44m55";
variable ind_p1 = where(p.name==sname_p1)[0];
variable ind_p2 = where(p.name==sname_p2)[0];
variable num_slaves = 1;
if((p.freeze[ind_p1] == 0) && (p.freeze[ind_p2] == 0))
{
  variable name_p1 = sprintf("%s.%s", get_fit_fun, sname_p1);
  variable name_p2 = sprintf("%s.%s", get_fit_fun, sname_p2);
  variable sname_conf = sprintf("conf_%s_%s", sname_p1, sname_p2);
  system(sprintf("rm %s.log", sname_conf));
  system(sprintf("rm %s*.dat", sname_conf));
  system(sprintf("rm %s.fits", sname_conf));
  variable nx = 33;
  variable ny = 33;
  conf_map = get_confmap(name_p1,
                         max([get_par(name_p1)/1.3, min(photometry_fit->grid_info.coverage[0].t), p.min[ind_p1]]),
                         min([get_par(name_p1)*1.3, max(photometry_fit->grid_info.coverage[0].t), p.max[ind_p1]]), nx,
                         name_p2,
                         max([get_par(name_p2)-0.2, p.min[ind_p2], 0]),
                         min([get_par(name_p2)+0.2, p.max[ind_p2]]), ny;
                         flood, save=sname_conf, num_slaves=num_slaves);
  xfig_plot_confmap(sname_conf+".fits"; chi2=[1, 2.71, 6.63],
                    chi2_max=100., colormap="haxby",
%                    chi2_max=50., colormap="gray",
                    xlabel="$T_{\mathrm{eff}}\,(\mathrm{K})$"R,
                    ylabel="$E(44-55)\,(\mathrm{mag})$"R,
                    zlabel="$\Delta \chi^2$"R, best_fit).render(sname_conf+".pdf");
}
sname_p1 = "c1_teff";
sname_p2 = "c1_logg";
ind_p1 = where(p.name==sname_p1)[0];
ind_p2 = where(p.name==sname_p2)[0];
if((p.freeze[ind_p1] == 0) && (p.freeze[ind_p2] == 0))
{
  name_p1 = sprintf("%s.%s", get_fit_fun, sname_p1);
  name_p2 = sprintf("%s.%s", get_fit_fun, sname_p2);
  sname_conf = sprintf("conf_%s_%s", sname_p1, sname_p2);
  system(sprintf("rm %s.log", sname_conf));
  system(sprintf("rm %s*.dat", sname_conf));
  system(sprintf("rm %s.fits", sname_conf));
  nx = 21; ny = 21;
  conf_map = get_confmap(name_p1,
                         max([get_par(name_p1)/1.3, min(photometry_fit->grid_info.coverage[0].t), p.min[ind_p1]]),
                         min([get_par(name_p1)*1.3, max(photometry_fit->grid_info.coverage[0].t), p.max[ind_p1]]), nx,
                         name_p2,
                         max([get_par(name_p2)-1.3, min(photometry_fit->grid_info.coverage[0].g), p.min[ind_p2]]),
                         min([get_par(name_p2)+1.3, max(photometry_fit->grid_info.coverage[0].g), p.max[ind_p2]]), ny;
                         flood, save=sname_conf, num_slaves=num_slaves);
  xfig_plot_confmap(sname_conf+".fits"; chi2=[1, 2.71, 6.63],
                    chi2_max=100., colormap="haxby",
%                    chi2_max=50., colormap="gray",
                    xlabel="$T_{\mathrm{eff}}\,(\mathrm{K})$"R,
                    ylabel="$\log (g\,\mathrm{(cm\,s^{-2})})$"R,
                    zlabel="$\Delta \chi^2$"R, best_fit).render(sname_conf+".pdf");
}
#endif

vmessage(sprintf("- script completed in %.1fs", _ftime-tscript_start));

exit;
