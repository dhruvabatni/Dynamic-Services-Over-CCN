/*
 * A CCNx command line utility.
 *
 * Copyright (C) 2008, 2009 Palo Alto Research Center, Inc.
 *
 * This work is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation. 
 * This work is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details. You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

package org.ccnx.ccn.utils;

import java.io.IOException;
import java.security.InvalidKeyException;
import java.util.logging.Level;

import org.ccnx.ccn.CCNHandle;
import org.ccnx.ccn.config.ConfigurationException;
import org.ccnx.ccn.impl.support.Log;
import org.ccnx.ccn.profiles.VersioningProfile;
import org.ccnx.ccn.profiles.metadata.MetadataProfile;
import org.ccnx.ccn.protocol.ContentName;
import org.ccnx.ccn.protocol.MalformedContentNameStringException;

/**
 * Command-line utility to write metadata associated with an existing file in ccnd. The "metaname" should
 * be the relative path (including filename) for the desired metadata only.
 * By default this writes to the repo. Otherwise there must be a corresponding ccngetfile to retrieve
 * the data.
 **/
 public class ccnputmeta extends CommonOutput {

	/**
	 * @param args
	 */
	public void write(String[] args) {
		Log.setDefaultLevel(Level.WARNING);
		int startArg = 0;
		
		for (int i = 0; i < args.length - 3; i++) {
			if (args[i].equals(("-raw"))) {
				if (startArg <= i)
					startArg = i + 1;
				CommonParameters.rawMode = true;
			} else if (args[i].equals("-unversioned")) {
				if (startArg <= i)
					startArg = i + 1;
				CommonParameters.unversioned = true;
			} else if (args[i].equals("-timeout")) {
				if (args.length < (i + 2)) {
					usage();
				}
				try {
					CommonParameters.timeout = Integer.parseInt(args[++i]);
				} catch (NumberFormatException nfe) {
					usage();
				}
				if (startArg <= i)
					startArg = i + 1;
			} else if (args[i].equals("-log")) {
				Level level = null;
				if (args.length < (i + 2)) {
					usage();
				}
				try {
					level = Level.parse(args[++i]);
				} catch (NumberFormatException nfe) {
					usage();
				}
				Log.setLevel(level);
				if (startArg <= i)
					startArg = i + 1;
			} else if (args[i].equals("-v")) {
				CommonParameters.verbose = true;
				if (startArg <= i)
					startArg = i + 1;
			} else if (args[i].equals("-as")) {
				if (args.length < (i + 2)) {
					usage();
				}
				CommonSecurity.setUser(args[++i]);
				if (startArg <= i)
					startArg = i + 1;				
			} else if (args[i].equals("-ac")) {
				CommonSecurity.setAccessControl();
				if (startArg <= i)
					startArg = i + 1;				
			}
			else {
				usage();
			}
				
		}
		
		if (args.length != startArg + 3) {
			usage();
		}
		
		long starttime = System.currentTimeMillis();
		try {
			// If we get one file name, put as the specific name given.
			// If we get more than one, put underneath the first as parent.
			// Ideally want to use newVersion to get latest version. Start
			// with random version.
			
			ContentName baseName = ContentName.fromURI(args[startArg]);
			String metaArg = args[startArg + 1];
			if (!metaArg.startsWith("/"))
				metaArg = "/" + metaArg;
			ContentName metaPath = ContentName.fromURI(metaArg);
			CCNHandle handle = CCNHandle.open();
			ContentName prevFileName = MetadataProfile.getLatestVersion(baseName, metaPath, CommonParameters.timeout, handle);
			if (null == prevFileName) {
				System.out.println("File " + baseName + " does not exist");
				System.exit(1);
			}
			ContentName fileName = VersioningProfile.updateVersion(prevFileName);
			if (CommonParameters.verbose)
				Log.info("ccnputmeta: putting metadata file " + args[startArg + 1]);
			
			doPut(handle, args[startArg + 2], fileName);
			System.out.println("Inserted metadata file: " + args[startArg + 1] + " for file: " + args[startArg] + ".");
			if (CommonParameters.verbose)
				System.out.println("ccnputmeta took: "+(System.currentTimeMillis() - starttime)+" ms");
			System.exit(0);
		} catch (ConfigurationException e) {
			System.out.println("Configuration exception in put: " + e.getMessage());
			e.printStackTrace();
		} catch (MalformedContentNameStringException e) {
			System.out.println("Malformed name: " + args[startArg] + " " + e.getMessage());
			e.printStackTrace();
		} catch (IOException e) {
			System.out.println("Cannot read file. " + e.getMessage());
			e.printStackTrace();
		} catch (InvalidKeyException e) {
			System.out.println("Cannot publish invalid key: " + e.getMessage());
			e.printStackTrace();
		}
		System.exit(1);

	}
	
	public void usage() {
		System.out.println("usage: ccnputmeta [-v (verbose)] [-raw] [-unversioned] [-timeout millis] [-log level] [-as pathToKeystore] [-ac (access control)] <ccnname> <metaname> (<filename>|<url>)*");
		System.exit(1);
	}
	
	public static void main(String[] args) {
		new ccnputmeta().write(args);
	}
}
