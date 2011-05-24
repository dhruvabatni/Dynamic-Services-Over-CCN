/*
 * Part of the CCNx Java Library.
 *
 * Copyright (C) 2008, 2009 Palo Alto Research Center, Inc.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation. 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details. You should have received
 * a copy of the GNU Lesser General Public License along with this library;
 * if not, write to the Free Software Foundation, Inc., 51 Franklin Street,
 * Fifth Floor, Boston, MA 02110-1301 USA.
 */

package org.ccnx.ccn.profiles.metadata;

import java.io.IOException;
import java.util.ArrayList;

import org.ccnx.ccn.CCNHandle;
import org.ccnx.ccn.profiles.CCNProfile;
import org.ccnx.ccn.profiles.CommandMarker;
import org.ccnx.ccn.protocol.ContentName;

/**
 * Access metadata files in the thumbnail namespace
 */
public class ThumbnailProfile implements CCNProfile {
	
	public static final CommandMarker THUMBNAIL_MARKER = 
		CommandMarker.commandMarker(MetadataProfile.METADATA_NAMESPACE, "thumbnail");
	
	private static class ThumbnailNamer implements MetadataProfile.MetaNamer {
		public ContentName getMetaName(ContentName baseName,ArrayList<byte[]> metaName) {
			return new ContentName(thumbnailName(baseName), metaName);
		}
	}
	
	/**
	 * Get the preset directory level name for metadata for thumbnails based on a base file
	 * @param baseName the base file as a ContentName
	 * @return the thumbnail meta directory as a ContentName
	 */
	public static ContentName thumbnailName(ContentName baseName) {
		return new ContentName(MetadataProfile.metadataName(baseName), THUMBNAIL_MARKER.getBytes());
	}
	
	/**
	 * Get the latest version of a thumbnail metadata file which is associated with a base file. 
	 * Before searching for the thumbnail version, we find the latest version of the base file
	 * 
	 * @param baseName the base file as a ContentName
	 * @param metaName the thumbnail filename as a byte array
	 * @param timeout  time to search for the latest version in ms. Applies separately to each latest
	 *                 version search.
	 * @param handle   CCNHandle to use for search.
	 * @return
	 * @throws IOException
	 */
	public static ContentName getLatestVersion(ContentName baseName, byte[] metaName, long timeout, CCNHandle handle) throws IOException {
		ArrayList<byte[]> list = new ArrayList<byte[]>();
		list.add(metaName);
		return MetadataProfile.getLatestVersion(baseName, new ThumbnailNamer(), list, timeout, handle);
	}
}
