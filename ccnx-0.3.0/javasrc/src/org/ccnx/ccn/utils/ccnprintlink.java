/*
 * A CCNx command line utility.
 *
 * Copyright (C) 2010 Palo Alto Research Center, Inc.
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

import java.util.logging.Level;

import org.ccnx.ccn.CCNHandle;
import org.ccnx.ccn.KeyManager;
import org.ccnx.ccn.impl.support.Log;
import org.ccnx.ccn.io.content.ContentDecodingException;
import org.ccnx.ccn.io.content.Link.LinkObject;
import org.ccnx.ccn.protocol.ContentName;

public class ccnprintlink {

	public static void usage() {
		System.err.println("usage: ccnlink [-q] <link uri> [<link uri> ...]  (-q == quiet)");
	}
	/**
	 * @param args
	 */
	public static void main(String[] args) {
		try {
			
			if (args.length < 1) {
				usage();
				return;
			}
			CCNHandle handle = CCNHandle.getHandle();
			
			int offset = 0;
			if ((args.length > 1) && (args[0].equals("-q"))) {
				Log.setDefaultLevel(Level.WARNING);
				offset++;
			}

			ContentName linkName = null;
			LinkObject linkObject = null;
			for (int i=offset; i < args.length; ++i) {
				try {
				linkName = ContentName.fromURI(args[i]);
				linkObject = new LinkObject(linkName, handle);
				if (linkObject.available()) {
					System.out.println("Link: " + linkObject);
				} else {
					System.out.println("No data available at " + linkName);
				}
				} catch (ContentDecodingException e) {
					System.out.println(linkName + " is not a link: " + e.getMessage());
				}
			}
			
			handle.close();
			handle.keyManager().close();
			KeyManager.closeDefaultKeyManager();

		} catch (Exception e) {
			handleException("Error: cannot initialize device. ", e);
			System.exit(-3);
		}
	}

	protected static void handleException(String message, Exception e) {
		Log.warning(message + " Exception: " + e.getClass().getName() + ": " + e.getMessage());
		Log.warningStackTrace(e);
	}
}
