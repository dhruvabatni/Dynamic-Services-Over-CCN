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
import org.ccnx.ccn.impl.CCNFlowControl.SaveType;
import org.ccnx.ccn.impl.support.Log;
import org.ccnx.ccn.impl.support.Tuple;
import org.ccnx.ccn.io.content.Link;
import org.ccnx.ccn.io.content.Link.LinkObject;
import org.ccnx.ccn.protocol.ContentName;

/**
 * Command line utility for making links. Currently does not take authenticator
 * information, just target name.
 * TODO add ability to specify authenticators
 */
public class ccnlink {

	public static void usage() {
		System.err.println("usage: ccnlink [-q] [-r] <link uri> <link target uri> [-as <pathToKeystore> [-name <friendly name]] (-q == quiet, -r == raw)");
	}
	/**
	 * @param args
	 */
	public static void main(String[] args) {
		try {
			
			int offset = 0;
			if ((args.length > 1) && (args[0].equals("-q"))) {
				Log.setDefaultLevel(Level.WARNING);
				offset++;
			}
			
			SaveType type = SaveType.REPOSITORY;
			if ((args.length-offset > 1) && (args[0].equals("-r"))) {
				type = SaveType.RAW;	
				offset++;
			}

			if (args.length-offset < 2) {
				usage();
				return;
			}

			ContentName linkName = ContentName.fromURI(args[offset++]);
			ContentName targetName = ContentName.fromURI(args[offset++]);

			Tuple<Integer, CCNHandle> tuple = CreateUserData.handleAs(args, offset);

			// Can also use command line system properties and environment variables to
			// point this handle to the correct user.
			CCNHandle handle = ((null == tuple) || (null == tuple.second())) ? CCNHandle.getHandle() : tuple.second();

			LinkObject theLink = new LinkObject(linkName, new Link(targetName), type, handle);
			theLink.save();
			theLink.close();
			
			System.out.println("Created link: " + theLink);
			
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
