// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#import "github.h"

@interface CommitListPresenter : NSObject

- (id)init:(UITableView*)tableView;

- (bool)refresh;
- (void)reset;

- (int)commitCount;
- (CommitNode*)commitAtIndex:(int)index;

@end